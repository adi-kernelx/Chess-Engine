import { ml_kem768, ml_dsa65 } from '../vendor/pqc.min.js';

export const pqcProvider = {
    mlKemEncapsulate(publicKey) {
        const { cipherText, sharedSecret } = ml_kem768.encapsulate(publicKey);
        return { ct: cipherText, sharedSecret };
    },
    mlDsaVerify(publicKey, message, signature) {
        return ml_dsa65.verify(signature, message, publicKey);
    },
    async x25519Generate() {
        const pair = await crypto.subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
        return { privateKey: pair.privateKey,
            publicKey: new Uint8Array(await crypto.subtle.exportKey('raw', pair.publicKey)) };
    },
    async x25519Derive(privateKey, peerPublic) {
        const publicKey = await crypto.subtle.importKey('raw', peerPublic, 'X25519', false, []);
        return new Uint8Array(await crypto.subtle.deriveBits(
            { name: 'X25519', public: publicKey }, privateKey, 256));
    },
};
