import assert from 'node:assert/strict';
import { consumeEmailCallback } from '../frontend/js/net/email-callback.js';
class Node {
    constructor(tag='',text=''){this.tag=tag;this.text=text;this.children=[];this.attributes={};this.handlers={};
        this.style={};this.dataset={};this.value='';this.disabled=false;
        this.classList={toggle:(name,on)=>{const names=new Set((this.attributes.class||'').split(' '));
            if(on)names.add(name);else names.delete(name);this.attributes.class=[...names].join(' ');}};}
    setAttribute(k,v){this.attributes[k]=String(v);}
    removeAttribute(k){delete this.attributes[k];}
    addEventListener(k,fn){this.handlers[k]=fn;}
    appendChild(n){this.children.push(n);n.parentNode=this;}
    removeChild(n){this.children.splice(this.children.indexOf(n),1);}
    get firstChild(){return this.children[0];}
    get textContent(){return this.text+this.children.map(n=>n.textContent).join('');}
    set textContent(v){this.text=v;this.children=[];}
    querySelector(){return null;}
    focus(){this.focused=true;}
    checkValidity(){return this.attributes.type!=='email' || /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(this.value);}
}
globalThis.Node=Node;
globalThis.document={createElement:t=>new Node(t),createTextNode:t=>new Node('',t),createElementNS:(_,t)=>new Node(t),
    readyState:'loading',addEventListener(){}};
globalThis.location={hostname:'localhost',origin:'http://localhost:8000'};
const { EmailRecoveryScreen, validNewPassword }=await import('../frontend/js/screens/email-recovery.js');
for(const weak of ['Ab1!xyz','password1!','Password!','Password1','Password1 ','A1!'+ 'x'.repeat(254)])
    assert.equal(validNewPassword(weak),false);
assert.equal(validNewPassword('Abcdef1!'),true);
assert.equal(validNewPassword('A1!'+ 'x'.repeat(253)),true);
const { AuthScreen }=await import('../frontend/js/screens/auth.js');
const { Outbound }=await import('../frontend/js/net/protocol.js');
await import('../frontend/js/main.js');
await import('../frontend/js/screens/profile.js');
let url='';const history={replaceState:(_,__,value)=>url=value};
const token='a'.repeat(43);
for(const mode of ['verify-email','activate-account','reset-password']) {
    const action=consumeEmailCallback({hash:'#/'+mode+'/'+token,pathname:'/',search:''},history);
    assert.deepEqual(action,{mode,token});assert.equal(url,'/#/'+mode);assert.ok(!url.includes(token));
}
assert.equal(consumeEmailCallback({hash:'#/reset-password/not-a-valid-token',pathname:'/',search:''},history).token,'');
assert.equal(url,'/#/reset-password');
assert.equal(consumeEmailCallback({hash:'#/login'},history),null);
let calls=[],adoptions=0,response={ok:true,data:{type:'email_sent'}},loggedOut=0,navigated=null;
const context=()=>({store:{session:{username:''}},emailAction:null,
    session:{isAuthenticated:false,accessToken:'synthetic',adopt(){adoptions++;},async logout(){loggedOut++;}},
    authClient:{async request(msg){calls.push(msg);return response;}},
    toast:Object.fromEntries(['warning','error','success','info'].map(k=>[k,()=>{}])),router:{go(path){navigated=path;}},Outbound});
const mount=(mode,action)=>{const ctx=context();ctx.emailAction=action;const screen=new EmailRecoveryScreen(ctx,mode);
    screen.root=screen.render();screen.onMount();return screen;};
let screen=mount('forgot-password');screen.email.value='bad';await screen._submit();assert.equal(calls.length,0);assert.match(screen.error.textContent,/valid email/);
screen.email.value='synthetic@example.com';await screen._submit();assert.equal(calls.at(-1).type,'request_password_reset');
assert.match(screen.status.textContent,/If this address is eligible/);assert.equal(adoptions,0);
response={ok:false,code:'unavailable'};await screen._submit();assert.equal(screen.button.disabled,false);assert.match(screen.error.textContent,/Reconnect/);
screen=mount('reset-password');assert.equal(screen.button.disabled,true);assert.match(screen.error.textContent,/original email/);
screen=mount('reset-password',{mode:'reset-password',token});assert.equal(screen.ctx.emailAction,null);
for(const weak of ['Ab1!xyz','password1!','Password!','Password1']) {
    screen.password.value=weak;screen.confirm.value=weak;const before=calls.length;await screen._submit();
    assert.equal(calls.length,before);assert.match(screen.error.textContent,/uppercase letter, a number, and a special character/);
}
screen.password.value='New-password1!';screen.confirm.value='different';const count=calls.length;await screen._submit();assert.equal(calls.length,count);
screen.confirm.value='New-password1!';response={ok:false,code:'invalid_email_token'};await screen._submit();assert.match(screen.error.textContent,/expired/);
response={ok:true,data:{type:'auth_action_ok'}};screen.ctx.session.isAuthenticated=true;await screen._submit();
assert.equal(calls.at(-1).email_token,token);assert.equal(screen.token,'');assert.equal(screen.button.disabled,true);assert.equal(loggedOut,1);
screen=mount('activate-account',{mode:'activate-account',token});screen.password.value='Mailbox-password1!';screen.confirm.value='Mailbox-password1!';
screen.ctx.authClient.request=async msg=>{calls.push(msg);return msg.type==='verify_email'
    ? {ok:true,data:{type:'auth_action_ok',username:'Fixture'}}
    : {ok:true,data:{type:'auth_ok',username:'Fixture',elo:800,access_token:'synthetic',refresh_token:'synthetic',access_expires_in:900}};};
await screen._submit();assert.equal(calls.at(-2).type,'verify_email');assert.equal(calls.at(-1).type,'login');
assert.equal(calls.at(-1).password,'Mailbox-password1!');assert.equal(calls.at(-1).username,'Fixture');
assert.equal(adoptions,1);assert.equal(navigated,'/');assert.equal(screen.token,'');assert.equal(screen.password.value,'');
screen=mount('activate-account',{mode:'activate-account',token});screen.password.value='Mailbox-password1!';screen.confirm.value='Mailbox-password1!';
screen.ctx.authClient.request=async msg=>{calls.push(msg);return msg.type==='verify_email'
    ? {ok:true,data:{type:'auth_action_ok',username:'Fixture'}} : {ok:false,code:'unavailable'};};
await screen._submit();assert.equal(adoptions,1);assert.match(screen.error.textContent,/Account activated.*Automatic sign-in failed/);
assert.equal(screen.button.disabled,true);assert.equal(screen.password.value,'');assert.equal(screen.token,'');
screen=mount('verify-email',{mode:'verify-email',token});
await screen._submit();assert.deepEqual(calls.at(-1),{type:'verify_email',email_token:token});
screen=mount('recovery-email');assert.equal(screen.button.disabled,true);
const ctx=context();ctx.session.isAuthenticated=true;screen=new EmailRecoveryScreen(ctx,'recovery-email');screen.root=screen.render();screen.onMount();
screen.email.value='old@example.com';screen.password.value='old-password';await screen._submit();assert.equal(calls.at(-1).access_token,'synthetic');
screen.onUnmount();assert.equal(screen.token,'');
const auth=new AuthScreen(context(),'register');auth.root=auth.render();auth._userInput.value='Fixture';assert.equal(auth._passInput,null);
auth._emailInput.value='invalid';const before=calls.length;await auth._submit();assert.equal(calls.length,before);
auth._emailInput.value='fixture@example.com';response={ok:true,data:{type:'email_sent'}};await auth._submit();
assert.equal(calls.at(-1).email,'fixture@example.com');assert.equal(adoptions,1);assert.match(auth._status.textContent,/not signed in yet/);
assert.equal(Object.hasOwn(calls.at(-1),'password'),false);
console.log('PASS email forms: imports, URL scrubbing, activation, reset, registration email, errors, recovery, no implicit login/storage');
