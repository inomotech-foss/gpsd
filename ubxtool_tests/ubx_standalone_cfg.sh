echo Start ubx_standalone_cfg script

#set up verbosity level & protocol
export UBXOPTS="-v 2 -P 23.01"

#display ubxtool version
ubxtool -V
#disable NMEA
ubxtool -d NMEA
#disable GLONASS
ubxtool -d GLONASS
#disable SBAS
ubxtool -e SBAS
#disable basic BINARY messages
ubxtool -d BINARY
#disable sending of ECEF binary messages.
ubxtool -d ECEF
#enable PPS
#ubxtool -e PPS

#disable UBX_NAV_SOL
ubxtool -c 0x06,0x01,0x01,0x06,0,0,0,0,0,0
#disable UBX_NAV_TIMELS
ubxtool -c 0x06,0x01,0x01,0x26,0,0,0,0,0,0
#disable all INF messages
ubxtool -c 0x06,0x02,0,0,0,0,0,0,0,0,0,0

#enable autonomous AssitNow, set initial fix as 3D
ubxtool -c 0x06,0x23,0x02,0,0x40,0x40,0,0,0,0,0,0,0,0,0,0,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0x01,0,0,0,0x64,0,0,0,0,0,0,0,0

#setup super E-mode 1Hz aggressive
ubxtool -c 0x06,0x86,0,0x03,0,0,0,0,0,0
#enable power save mode on CFG-RXM
ubxtool -c 0x06,0x11,0,0x01

#ERASE log and disable logger
ubxtool -p LOG-ERASE

##configure LOGFILTER
#flags >> recorEnabled, applyAllFilters = 0x05
#minInterval >> 118s=0x76
#timeThd >> 120s=0x0078
#speedThd >> None=0
#distanceThd >> 500m=0x1F4
#ubxtool -c 0x06,0x47,0x01,0x05,0x76,0x00,0x78,0x00,0,0,0xF4,0x01,0,0

#test config w/ 10seconds between logs
ubxtool -c 0x06,0x47,0x01,0x05,0x09,0x00,0x0A,0x00,0,0,0xF4,0x01,0,0

#CREATELOG and start logger (circular buffer config)
#ubxtool -p LOG-CREATE
ubxtool -c 0x021,0x07,0,0x01,0,0,0,0,0,0

echo ubx_standalone_cfg complete
