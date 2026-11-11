"""Container port per security type; shared by the fixture and its probe."""
# (container port, Xvnc SecurityTypes, RFB type the probe selects, VeNCrypt subtype or None)
PORTS = (
    (5900, 'VncAuth', 2, None),
    (5901, 'Plain', 19, 256),
    (5902, 'TLSPlain', 19, 259),
    (5903, 'X509Plain', 19, 262),
    (5904, 'TLSVnc', 19, 258),
    (5905, 'X509Vnc', 19, 261),
    # Offers every subtype at once, so the client's own preference order is exercised.
    (5906, 'X509Plain,TLSPlain,X509Vnc,TLSVnc,Plain,VncAuth', 19, 262),
)
USER = 'vncfixture'
