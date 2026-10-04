import { Screen } from '../ui/screen.js';
import { h } from '../core/dom.js';

const COPY={
    invalid_email:'Enter a valid email address.', weak_password:'Use a password of 8–256 characters.',
    invalid_email_token:'This link has expired or was already used. Request a new link.',
    email_conflict:'This username or email is already attached to an account. Sign in with Google or use Forgot password.',
    invalid_credentials:'Your current password could not be verified.',
    unauthorized:'Sign in again before adding a recovery email.',
    rate_limited:'Too many attempts. Please wait a minute before trying again.',
    email_unavailable:'Email delivery is unavailable. Please try again later.',
    unavailable:'The server did not respond. Reconnect and try again.',
};
export class EmailRecoveryScreen extends Screen {
    constructor(ctx, mode) {
        super(ctx); this.mode=mode; this.busy=false;
        this.token=ctx.emailAction?.mode===mode ? ctx.emailAction.token : '';
        if(this.token) ctx.emailAction=null;
    }
    render() {
        const titles={'forgot-password':'Forgot password','reset-password':'Set a new password','activate-account':'Activate your account',
            'verify-email':'Verify your email','recovery-email':'Add recovery email'};
        const wantsEmail=this.mode==='forgot-password' || this.mode==='recovery-email';
        const wantsPassword=['reset-password','activate-account','recovery-email'].includes(this.mode);
        return h('div',{class:'screen'},this.header(titles[this.mode],'Keep your account and game history secure.'),
            h('div',{class:'screen__body'},h('div',{class:'auth-wrap'},h('div',{class:'card auth-card'},
                h('div',{class:'card__body field-stack auth-form'},
                    h('p',{class:'auth-note'},this.mode==='recovery-email'
                        ? 'For existing password accounts without a recovery email. Confirm your current password, then verify the email link.'
                        : this.mode==='forgot-password' ? 'Enter your verified account email. The response is the same whether an account exists or not.'
                        : 'Links are single-use. Opening this page does not change your account until you confirm.'),
                    h('div',{id:'recovery-error',role:'alert',tabindex:'-1',class:'field__error',ref:el=>this.error=el}),
                    wantsEmail ? this._field('Email','email','email',el=>this.email=el) : null,
                    wantsPassword ? this._field(this.mode==='recovery-email'?'Current password':'New password','password',
                        this.mode==='recovery-email'?'current-password':'new-password',el=>this.password=el) : null,
                    ['reset-password','activate-account'].includes(this.mode) ? this._field('Confirm new password','password','new-password',el=>this.confirm=el) : null,
                    h('button',{type:'button',class:'btn btn--primary btn--block',ref:el=>this.button=el,
                        onclick:()=>this._submit()},this.mode==='verify-email'?'Confirm email':this.mode==='activate-account'?'Activate account':this.mode==='reset-password'?'Save password':'Send email'),
                    h('div',{role:'status','aria-live':'polite',class:'auth-note',ref:el=>this.status=el}),
                    h('a',{class:'btn btn--ghost',href:'#/login'},'Back to sign in'),
                    h('a',{class:'btn btn--ghost',href:'#/forgot-password'},'Request a password link'))))));
    }
    _field(label,type,autocomplete,ref) {
        const id='recovery-'+label.toLowerCase().replaceAll(' ','-');
        return h('div',{class:'field'},h('label',{class:'field__label',for:id},label),
            h('input',{id,class:'input',type,autocomplete,'aria-describedby':'recovery-error',maxlength:type==='email'?254:256,
                ref,onkeydown:e=>{if(e.key==='Enter') this._submit();}}));
    }
    onMount() {
        if(['verify-email','reset-password','activate-account'].includes(this.mode) && !this.token) {
            this._error('Open the original email link again. Its token is not saved in browser storage.');
            this.button.disabled=true;
        } else if(this.mode==='recovery-email' && !this.ctx.session?.isAuthenticated) {
            this._error('Sign in first to add a recovery email.');this.button.disabled=true;
        } else (this.email || this.password || this.button)?.focus();
    }
    _error(message,field=null) {
        this.error.textContent='';
        if(field) {
            field.setAttribute('aria-invalid','true');
            this.error.appendChild(h('a',{href:'#'+(field.id||field.attributes.id),
                onclick:event=>{event.preventDefault();field.focus();}},message));
        } else this.error.textContent=message;
        this.error.focus();
    }
    async _submit() {
        if(this.busy || this.button.disabled) return;
        this.error.textContent='';
        for(const field of [this.email,this.password,this.confirm]) field?.removeAttribute('aria-invalid');
        if(this.email && (!this.email.value.trim() || !this.email.checkValidity())) return this._error(COPY.invalid_email,this.email);
        if(this.password && (this.password.value.length<8 || this.password.value.length>256)) return this._error(COPY.weak_password,this.password);
        if(this.confirm && this.password.value!==this.confirm.value) return this._error('Passwords do not match.',this.confirm);
        if(['verify-email','reset-password','activate-account'].includes(this.mode) && !this.token) return this._error(COPY.invalid_email_token);
        const msg=this.mode==='verify-email'?{type:'verify_email',email_token:this.token}:
            this.mode==='activate-account'?{type:'verify_email',email_token:this.token,password:this.password.value}:
            this.mode==='reset-password'?{type:'reset_password',email_token:this.token,password:this.password.value}:
            this.mode==='recovery-email'?{type:'set_recovery_email',email:this.email.value.trim(),password:this.password.value,
                access_token:this.ctx.session.accessToken}:{type:'request_password_reset',email:this.email.value.trim()};
        this.busy=true;this.button.disabled=true;this.button.setAttribute('aria-busy','true');
        let complete=false;
        try {
            const result=await this.ctx.authClient.request(msg);
            if(!result.ok) return this._error(COPY[result.code] || 'Could not complete this request. Please try again.');
            if(this.password) this.password.value='';if(this.confirm) this.confirm.value='';
            this.status.textContent=['verify-email','activate-account'].includes(this.mode)?'Email confirmed. You can now sign in.':
                this.mode==='reset-password'?'Password saved. Existing sessions were revoked. Sign in with your new password.':
                'If this address is eligible, an email will arrive shortly. Check Spam too. You can retry if it does not arrive.';
            if(['verify-email','reset-password','activate-account'].includes(this.mode)) {
                this.token='';complete=true;
                if(this.mode==='reset-password' && this.ctx.session?.isAuthenticated) await this.ctx.session.logout();
            }
        } catch {this._error(COPY.unavailable);}
        finally {this.busy=false;this.button.disabled=complete;this.button.removeAttribute('aria-busy');}
    }
    onUnmount() {this.token='';}
}
