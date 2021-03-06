[*Mpd 5.9 User Manual*](mpd.html) **:** [*Installation*](mpd5.html)
**:** *Building mpd*\
**Previous:** [*Installing mpd*](mpd7.html)\
**Next:** [*Running Mpd*](mpd9.html)

------------------------------------------------------------------------

## []{#8}2.3. Building mpd[]{#building}

If you choose to build mpd yourself to customize it, the process is
straightforward. First, edit the `Makefile` to define (or undefine) the
various device types and options you want to support. Run \'configure\'
script and then type `make depend all` to rebuild the binary.

The various build-time definitions in the Makefile are below:

**`MPD_CONF_DIR`**

:   The default configuration directory where mpd looks for `mpd.conf`,
    etc.

**`PHYSTYPE_MODEM`**

:   

**`PHYSTYPE_TCP`**

:   

**`PHYSTYPE_UDP`**

:   

**`PHYSTYPE_NG_SOCKET`**

:   

**`PHYSTYPE_PPTP`**

:   

**`PHYSTYPE_L2TP`**

:   

**`PHYSTYPE_PPPOE`**

:   Define these to include support for the corresponding device type.

**`ENCRYPTION_DES`**

:   These enable support for the corresponding encryption types.

**`SYSLOG_FACILITY`**

:   Mpd normally logs via `syslog(3)` using the facility `LOG_DAEMON`.
    You can customize the facility here.

------------------------------------------------------------------------

[*Mpd 5.9 User Manual*](mpd.html) **:** [*Installation*](mpd5.html)
**:** *Building mpd*\
**Previous:** [*Installing mpd*](mpd7.html)\
**Next:** [*Running Mpd*](mpd9.html)