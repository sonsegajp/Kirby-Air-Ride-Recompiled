"""Login cryptography (mirrors src/runtime/airnet.cpp).

The password never leaves the player's PC. The client derives a verifier with scrypt:

    verifier = scrypt(password_utf8, salt, n, r, p, dklen=32)

and registers (username, salt, n, r, p, verifier); the server stores the verifier. To log in, the
server hands out a one-time nonce and the client proves it knows the verifier without sending it:

    proof = HMAC-SHA256(verifier, "airride-login\\0" || server_nonce || client_nonce || username_key)

and the session token comes back masked with a key only the two of them can compute:

    token = token_enc XOR HMAC-SHA256(verifier, "airride-token\\0" || server_nonce || client_nonce)[:24]

Resuming a saved session works the same way with the token as the key:

    proof = HMAC-SHA256(token, "airride-resume\\0" || server_nonce || client_nonce || username_key)
"""

import hashlib
import hmac

from . import protocol as P


def derive_verifier(password, salt, n=P.SCRYPT_N, r=P.SCRYPT_R, p=P.SCRYPT_P):
    """The client's side of registration and login (the server itself never runs this)."""
    return hashlib.scrypt(password.encode("utf-8"), salt=salt, n=n, r=r, p=p,
                          maxmem=256 * 1024 * 1024, dklen=P.VERIFIER_BYTES)


def _hmac(key, *parts):
    return hmac.new(key, b"".join(parts), hashlib.sha256).digest()


def login_proof(verifier, server_nonce, client_nonce, key):
    return _hmac(verifier, b"airride-login\0", server_nonce, client_nonce, key.encode("utf-8"))


def resume_proof(token, server_nonce, client_nonce, key):
    return _hmac(token, b"airride-resume\0", server_nonce, client_nonce, key.encode("utf-8"))


def token_mask(verifier, server_nonce, client_nonce):
    return _hmac(verifier, b"airride-token\0", server_nonce, client_nonce)[:P.TOKEN_BYTES]


def xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def fake_salt(secret, key):
    """A stable salt for usernames that do not exist (login_begin does not reveal which exist)."""
    return _hmac(secret, b"airride-fake-salt\0", key.encode("utf-8"))[:P.SALT_BYTES]


def equal(a, b):
    return hmac.compare_digest(a, b)
