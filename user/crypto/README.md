# PPPoE CHAP digest

`md5.c` and `md5.h` are the independent public-domain RFC 1321 implementation
previously consumed from sbase commit `c1341583c96307cb0e6152c963ed23c4d56a4278`.
Only the local include path, header guard and standalone stdint include were changed. This preserves CHAP
interoperability without depending on the sbase command suite. MD5 is used for
the existing PPP CHAP protocol, not as a general-purpose security primitive.
