[*Mpd 5.9 User Manual*](mpd.html) **:** [*Internals*](mpd64.html) **:**
*ToDo*\
**Previous:** [*Internals*](mpd64.html)\
**Next:** [*Authentication*](mpd66.html)

------------------------------------------------------------------------

## []{#65}8.1. ToDo[]{#todo}

A list of open tasks for MPD. This is a good startpoint if you want to
support actively the development of MPD. After completing a task post it
to MPD\'s maling list.

**callback (client & server)**

:   Currently MPD supports only simpliest LCP callback without number
    negotiation, without CBCP support and only as client.

**L2TP auth proxying**

:   Currently MPD acting as LAC requires parameter renegotiation on
    every tunnel hop. It would be good to implement auth proxying to
    speedup negotiation.

**Unimplemented Protocols**

:   lcp.c contains a list of protocols (gLcpConfOpts), each protocol
    with \"false\" is not implemented.

------------------------------------------------------------------------

[*Mpd 5.9 User Manual*](mpd.html) **:** [*Internals*](mpd64.html) **:**
*ToDo*\
**Previous:** [*Internals*](mpd64.html)\
**Next:** [*Authentication*](mpd66.html)