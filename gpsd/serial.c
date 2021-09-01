/*
 * This file is Copyright 2010 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 */

#include "../include/gpsd_config.h"  // must be before all includes

#include <ctype.h>                   // for isdigit()
#include <dirent.h>                  // for DIR
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>               // defines BSD
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_SYS_SYSMACROS_H
    #include <sys/sysmacros.h>       // defines major()
#endif   // HAVE_SYS_SYSMACROS_H

#ifdef ENABLE_BLUEZ
    #include <bluetooth/bluetooth.h>
    #include <bluetooth/hci.h>
    #include <bluetooth/hci_lib.h>
    #include <bluetooth/rfcomm.h>
#endif   // ENABLE_BLUEZ

#include "../include/compiler.h"     // for FALLTHROUGH
#include "../include/gpsd.h"

// Workaround for HP-UX 11.23, which is missing CRTSCTS
#ifndef CRTSCTS
#  ifdef CNEW_RTSCTS
#    define CRTSCTS CNEW_RTSCTS
#  else
#    define CRTSCTS 0
#  endif  // CNEW_RTSCTS
#endif    // !CRTSCTS

// figure out what kind of device we're looking at
static sourcetype_t gpsd_classify(struct gps_device_t *session)
{
    struct stat sb;
    const char *path = session->gpsdata.dev.path;

    if (-1 == stat(path, &sb)) {
        GPSD_LOG(LOG_ERROR, &session->context->errout,
                 "SER: stat(%s) failed: %s(%d)\n",
                 session->gpsdata.dev.path, strerror(errno), errno);
        return SOURCE_UNKNOWN;
    }
    if (S_ISREG(sb.st_mode))
        return SOURCE_BLOCKDEV;

    // this assumes we won't get UDP from a filesystem socket
    if (S_ISSOCK(sb.st_mode))
        return SOURCE_TCP;

    // OS-independent check for ptys using Unix98 naming convention
    if (0 == strncmp(path, "/dev/pts/", 9))
        return SOURCE_PTY;

    // some more direct way to check for PPS?
    if (0 == strncmp(path, "/dev/pps", 8))
        return SOURCE_PPS;

    if (S_ISFIFO(sb.st_mode))
        return SOURCE_PIPE;

     if (S_ISCHR(sb.st_mode)) {
        sourcetype_t devtype = SOURCE_RS232;
#ifdef __linux__
        /* Linux major device numbers live here
         *
         * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/admin-guide/devices.txt
         *
         * Note: This code works because Linux major device numbers are
         * stable and architecture-independent.  It is *not* a good model
         * for other Unixes where either or both assumptions may break.
         */
        int devmajor = major(sb.st_rdev);
        int devminor = minor(sb.st_rdev);

        switch (devmajor) {
        case 3:      // First MFM, RLL and IDE hard disk/CD-ROM interface ?
            FALLTHROUGH
        case 136:    // Unix98 PTY slaves
            FALLTHROUGH
        case 137:    // Unix98 PTY slaves
            FALLTHROUGH
        case 138:    // Unix98 PTY slaves
            FALLTHROUGH
        case 139:    // Unix98 PTY slaves
            FALLTHROUGH
        case 140:    // Unix98 PTY slaves
            FALLTHROUGH
        case 141:    // Unix98 PTY slaves
            FALLTHROUGH
        case 142:    // Unix98 PTY slaves
            FALLTHROUGH
        case 143:    // Unix98 PTY slaves
            devtype = SOURCE_PTY;
            break;

        case 4:      // TTY Devices
            FALLTHROUGH
        case 204:    // Low-density serial ports
            FALLTHROUGH
        case 207:    // 207 FREESCALE I.MX UARTS (TTYMXC*)
            devtype = SOURCE_RS232;
            break;

        case 10:      // Non-serial mice, misc features
            if (223 == devminor) {
                devtype = SOURCE_PPS;
            } // else WTF?
            break;

        case 166:    // ACM USB modems
            // ACM has no speed, otherwise similar to SOURCE_USB
            devtype = SOURCE_ACM;
            break;
        case 188:    // USB serial converters
            devtype = SOURCE_USB;
            break;

        case 216:    // Bluetooth RFCOMM TTY devices
            FALLTHROUGH
        case 217:    // Bluetooth RFCOMM TTY devices (alternate devices)
            devtype = SOURCE_BLUETOOTH;
            break;

        default:     // Give up, default to rs232
            devtype = SOURCE_RS232;
            break;
        }
#endif  // __linux__
        /*
         * See http://nadeausoftware.com/articles/2012/01/c_c_tip_how_use_compiler_predefined_macros_detect_operating_system
         * for discussion how this works.  Key graphs:
         *
         * Compilers for the old BSD base for these distributions
         * defined the __bsdi__ macro, but none of these distributions
         * define it now. This leaves no generic "BSD" macro defined
         * by the compiler itself, but all UNIX-style OSes provide a
         * <sys/param.h> file. On BSD distributions, and only on BSD
         * distributions, this file defines a BSD macro that's set to
         * the OS version. Checking for this generic macro is more
         * robust than looking for known BSD distributions with
         * __DragonFly__, __FreeBSD__, __NetBSD__, and __OpenBSD__
         * macros.
         *
         * Apple's OSX for the Mac and iOS for iPhones and iPads are
         * based in part on a fork of FreeBSD distributed as
         * Darwin. As such, OSX and iOS also define the BSD macro
         * within <sys/param.h>. However, compilers for OSX, iOS, and
         * Darwin do not define __unix__. To detect all BSD OSes,
         * including OSX, iOS, and Darwin, use an #if/#endif that
         * checks for __unix__ along with __APPLE__ and __MACH__ (see
         * the later section on OSX and iOS).
         */
#ifdef BSD
        /*
         * Hacky check for pty, which is what really matters for avoiding
         * adaptive delay.
         */
        if (strncmp(path, "/dev/ttyp", 9) == 0 ||
            strncmp(path, "/dev/ttyq", 9) == 0)
            devtype = SOURCE_PTY;
        else if (strncmp(path, "/dev/ttyU", 9) == 0 ||
            strncmp(path, "/dev/dtyU", 9) == 0)
            devtype = SOURCE_USB;
        // XXX bluetooth
#endif  // BSD
        return devtype;
    }

    return SOURCE_UNKNOWN;
}

#ifdef __linux__

// return true if any process has the specified path open
static int fusercount(struct gps_device_t *session)
{
    const char *path = session->gpsdata.dev.path;
    DIR *procd, *fdd;
    struct dirent *procentry, *fdentry;
    char procpath[GPS_PATH_MAX], fdpath[GPS_PATH_MAX], linkpath[GPS_PATH_MAX];
    int cnt = 0;

    if (NULL == (procd = opendir("/proc"))) {
        GPSD_LOG(LOG_ERROR, &session->context->errout,
                 "SER: opendir(/proc) failed: %s(%d)\n",
                 strerror(errno), errno);
        return -1;
    }
    while (NULL != (procentry = readdir(procd))) {
        if (0 == isdigit(procentry->d_name[0]))
            continue;
        // longest procentry->d_name I could find was 12
        (void)snprintf(procpath, sizeof(procpath),
                       "/proc/%.20s/fd/", procentry->d_name);
        if (NULL == (fdd = opendir(procpath)))
            continue;
        while (NULL != (fdentry = readdir(fdd))) {
            ssize_t rd;

            (void)strlcpy(fdpath, procpath, sizeof(fdpath));
            (void)strlcat(fdpath, fdentry->d_name, sizeof(fdpath));
            // readlink does not NUL terminate.
            rd = readlink(fdpath, linkpath, sizeof(linkpath) - 1);
            if (0 > rd)
                continue;
            // pacify coverity by setting NUL
            linkpath[rd] = '\0';
            if (0 == strcmp(linkpath, path)) {
                ++cnt;
            }
        }
        (void)closedir(fdd);
    }
    (void)closedir(procd);

    return cnt;
}
#endif   // __linux__

// to be called on allocating a device
void gpsd_tty_init(struct gps_device_t *session)
{
    // mark GPS fd closed and its baud rate unknown
    session->gpsdata.gps_fd = -1;
    session->saved_baud = -1;
    session->zerokill = false;
    session->reawake = (time_t)0;
}

#if !defined(HAVE_CFMAKERAW)
/*
 * Local implementation of cfmakeraw (which is not specified by
 * POSIX; see matching declaration in gpsd.h).
 *
 * Pasted from man page; added in serial.c arbitrarily
 */
void cfmakeraw(struct termios *termios_p)
{
    termios_p->c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
}
#endif   // !defined(HAVE_CFMAKERAW)

// FIXME: convert speed2code() and code2speed() to be table driven

// Convert speed code into speed
static speed_t speed2code(const int speed)
{
    speed_t rate;

    if (1200 > speed)
        rate = B300;
    else if (2400 > speed)
        rate = B1200;
    else if (4800 > speed)
        rate = B2400;
    else if (9600 > speed)
        rate = B4800;
    else if (19200 > speed)
        rate = B9600;
    else if (38400 > speed)
        rate = B19200;
    else if (57600 > speed)
        rate = B38400;
    else if (115200 > speed)
        rate = B57600;
    else if (230400 > speed)
        rate = B115200;
#ifdef B460800
    else if (460800 > speed)
        // not a valid POSIX speed
        rate = B230400;
    else if (500000 > speed)
        rate = B460800;
#endif  // B460800
#ifdef B500000
    else if (576000 > speed)
        // not a valid POSIX speed
        rate = B500000;
#endif   // B500000
#ifdef B576000
    else if (921600 > speed)
        // not a valid POSIX speed
        rate = B576000;
#endif   // B500000
#ifdef B921600
    else if (1000000 > speed)
        // not a valid POSIX speed
        rate = B921600;
#endif   // B921600
#ifdef B1000000
    else if (1152000 > speed)
        // not a valid POSIX speed
        rate = B1000000;
#endif   // B1000000
#ifdef B1152000
    else if (1500000 > speed)
        // not a valid POSIX speed
        rate = B1152000;
#endif   // B1152000
#ifdef B1500000
    else if (2000000 > speed)
        // not a valid POSIX speed
        rate = B1500000;
#endif   // B1500000
#ifdef B2000000
    else if (2500000 > speed)
        // not a valid POSIX speed
        rate = B2000000;
#endif   // B2000000
#ifdef B2500000
    else if (3000000 > speed)
        // not a valid POSIX speed
        rate = B2500000;
#endif   // B2500000
#ifdef B3000000
    else if (3500000 > speed)
        // not a valid POSIX speed
        rate = B3500000;
#endif   // B3000000
#ifdef B3500000
    else if (4000000 > speed)
        // not a valid POSIX speed
        rate = B3500000;
#endif   // B3500000
#ifdef B4000000
    else if (4000000 == speed)
        // not a valid POSIX speed
        rate = B4000000;
#endif   // B4000000
    else {
        // we are confused
        rate = B9600;
    }
    return rate;
}

// Convert speed code into speed
static int code2speed(const speed_t code)
{
    switch (code) {
    case B300:
        return 300;
    case B1200:
        return 1200;
    case B2400:
        return 2400;
    case B4800:
        return 4800;
    case B9600:
        return 9600;
    case B19200:
        return 19200;
    case B38400:
        return 38400;
    case B57600:
        return 57600;
    case B115200:
        return 115200;
    case B230400:
        return 230400;
#ifdef B460800
    case B460800:
        // not a valid POSIX speed
        return 460800;
#endif
#ifdef B500000
    case B500000:
        // not a valid POSIX speed
        return 500000;
#endif
#ifdef B576000
    case B576000:
        // not a valid POSIX speed
        return 576000;
#endif
#ifdef B921600
    case B921600:
        // not a valid POSIX speed
        return 921600;
#endif
#ifdef B1000000
    case B1000000:
        // not a valid POSIX speed
        return 1000000;
#endif
#ifdef B1152000
    case B1152000:
        // not a valid POSIX speed
        return 1152000;
#endif
#ifdef B1500000
    case B1500000:
        // not a valid POSIX speed
        return 1500000;
#endif
    default:   // Unknown speed
        return 0;
    }
}

speed_t gpsd_get_speed(const struct gps_device_t *dev)
{
    return code2speed(cfgetospeed(&dev->ttyset));
}

speed_t gpsd_get_speed_old(const struct gps_device_t *dev)
{
    return code2speed(cfgetospeed(&dev->ttyset_old));
}

char gpsd_get_parity(const struct gps_device_t *dev)
{
    char parity = 'N';
    if ((dev->ttyset.c_cflag & (PARENB | PARODD)) == (PARENB | PARODD))
        parity = 'O';
    else if ((dev->ttyset.c_cflag & PARENB) == PARENB)
        parity = 'E';
    return parity;
}

int gpsd_get_stopbits(const struct gps_device_t *dev)
{
    int stopbits = 0;
    if ((dev->ttyset.c_cflag & CS8) == CS8)
        stopbits = 1;
    else if ((dev->ttyset.c_cflag & (CS7 | CSTOPB)) == (CS7 | CSTOPB))
        stopbits = 2;
    return stopbits;
}

bool gpsd_set_raw(struct gps_device_t * session)
{
    // on some OS cfmakeraw() returns an int, POSIX says it is void.
    (void)cfmakeraw(&session->ttyset);
    if (-1 == tcsetattr(session->gpsdata.gps_fd, TCIOFLUSH,
                        &session->ttyset)) {
        GPSD_LOG(LOG_ERROR, &session->context->errout,
                 "SER: error changing port attributes: %s(%d)\n",
                 strerror(errno), errno);
        return false;
    }

    return true;
}

// Set the port speed
void gpsd_set_speed(struct gps_device_t *session,
                    speed_t speed, char parity, unsigned int stopbits)
{
    speed_t rate;
    struct timespec delay;

    if (0 < session->context->fixed_port_speed) {
        speed = session->context->fixed_port_speed;
    }
    if ('\0' != session->context->fixed_port_framing[0]) {
        // ignore length, stopbits=2 forces length 7.
        parity = session->context->fixed_port_framing[1];
        stopbits = session->context->fixed_port_framing[2] - '0';
    }

    /*
     * Yes, you can set speeds that aren't in the hunt loop.  If you
     * do this, and you aren't on Linux where baud rate is preserved
     * across port closings, you've screwed yourself. Don't do that!
     * Setting the speed to B0 instructs the modem to "hang up".
     */
    rate = speed2code(speed);

    // backward-compatibility hack
    switch (parity) {
    case 'E':
        FALLTHROUGH
    case (char)2:
        parity = 'E';
        break;
    case 'O':
        FALLTHROUGH
    case (char)1:
        parity = 'O';
        break;
    case 'N':
        FALLTHROUGH
    case (char)0:
        FALLTHROUGH
    default:
        parity = 'N';   // without this we might emit malformed JSON
        break;
    }

    if (rate != cfgetispeed(&session->ttyset)
        || parity != session->gpsdata.dev.parity
        || stopbits != session->gpsdata.dev.stopbits) {

        /*
         *  "Don't mess with this conditional! Speed zero is supposed to mean
         *   to leave the port speed at whatever it currently is."
         *
         * The Linux man page says:
         *  "Setting the speed to B0 instructs the modem to "hang up".
         *
         * We use B0 as an internal flag to leave the speed alone.
         * This leads
         * to excellent behavior on Linux, which preserves baudrate across
         * serial device closes - it means that if you've opened this
         * device before you typically don't have to hunt at all because
         * it's still at the same speed you left it - you'll typically
         * get packet lock within 1.5 seconds.  Alas, the BSDs and OS X
         * aren't so nice.
         */
        if (rate == B0) {
            GPSD_LOG(LOG_IO, &session->context->errout,
                     "SER: keeping old speed %d(%d)\n",
                     code2speed(cfgetispeed(&session->ttyset)),
                     cfgetispeed(&session->ttyset));
        } else {
            (void)cfsetispeed(&session->ttyset, rate);
            (void)cfsetospeed(&session->ttyset, rate);
            GPSD_LOG(LOG_IO, &session->context->errout,
                     "SER: set speed %d(%d)\n",
                     code2speed(cfgetispeed(&session->ttyset)), rate);
        }
        session->ttyset.c_iflag &= ~(PARMRK | INPCK);
        session->ttyset.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
        session->ttyset.c_cflag |= (stopbits == 2 ? CS7 | CSTOPB : CS8);
        switch (parity) {
        case 'E':
            session->ttyset.c_iflag |= INPCK;
            session->ttyset.c_cflag |= PARENB;
            break;
        case 'O':
            session->ttyset.c_iflag |= INPCK;
            session->ttyset.c_cflag |= PARENB | PARODD;
            break;
        }
        if (0 != tcsetattr(session->gpsdata.gps_fd, TCSANOW,
                           &session->ttyset)) {
            /* strangely this fails on non-serial ports, but if
             * we do not try, we get other failures.
             * so ignore for now, as we always have, until it can
             * be nailed down.
             */
             GPSD_LOG(LOG_PROG, &session->context->errout,
                      "SER: error setting port attributes: %s(%d), "
                      "sourcetype: %d\n",
                      strerror(errno), errno, session->sourcetype);
        }

        /*
         * Serious black magic begins here.  Getting this code wrong can cause
         * failures to lock to a correct speed, and not clean reproducible
         * failures but flukey hardware- and timing-dependent ones.  So
         * be very sure you know what you're doing before hacking it, and
         * test thoroughly.
         *
         * The fundamental problem here is that serial devices take time
         * to settle into a new baud rate after tcsetattr() is issued. Until
         * they do so, input will be arbitrarily garbled.  Normally this
         * is not a big problem, but in our hunt loop the garbling can trash
         * a long enough prefix of each sample to prevent detection of a
         * packet header.  We could address the symptom by making the sample
         * size enough larger that subtracting the maximum length of garble
         * would still leave a sample longer than the maximum packet size.
         * But it's better (and more efficient) to address the disease.
         *
         * In theory, one might think that not even a tcflush() call would
         * be needed, with tcsetattr() delaying its return until the device
         * is in a good state.  For simple devices like a 14550 UART that
         * have fixed response timings this may even work, if the driver
         * writer was smart enough to delay the return by the right number
         * of milliseconds after poking the device port(s).
         *
         * Problems may arise if the driver's timings are off.  Or we may
         * be talking to a USB device like the pl2303 commonly used in GPS
         * mice; on these, the change will not happen immediately because
         * it has to be sent as a message to the external processor that
         * has to act upon it, and that processor may still have buffered
         * data in its own FIFO.  In this case the expected delay may be
         * too large and too variable (depending on the details of how the
         * USB device is integrated with its symbiont hardware) to be put
         * in the driver.
         *
         * So, somehow, we have to introduce a delay after tcsatattr()
         * returns sufficient to allow *any* device to settle.  On the other
         * hand, a really long delay will make gpsd device registration
         * unpleasantly laggy.
         *
         * The classic way to address this is with a tcflush(), counting
         * on it to clear the device FIFO. But that call may clear only the
         * kernel buffers, not the device's hardware FIFO, so it may not
         * be sufficient by itself.
         *
         * flush followed by a 200-millisecond delay followed by flush has
         * been found to work reliably on the pl2303.  It is also known
         * from testing that a 100-millisec delay is too short, allowing
         * occasional failure to lock.
         */
        (void)tcflush(session->gpsdata.gps_fd, TCIOFLUSH);

        // wait 200,000 uSec
        delay.tv_sec = 0;
        delay.tv_nsec = 200000000L;
        nanosleep(&delay, NULL);
        (void)tcflush(session->gpsdata.gps_fd, TCIOFLUSH);
    }
    GPSD_LOG(LOG_INF, &session->context->errout,
             "SER: current speed %lu, %d%c%d\n",
             (unsigned long)gpsd_get_speed(session), 9 - stopbits, parity,
             stopbits);

    session->gpsdata.dev.baudrate = (unsigned int)speed;
    session->gpsdata.dev.parity = parity;
    session->gpsdata.dev.stopbits = stopbits;

    /*
     * The device might need a wakeup string before it will send data.
     * If we don't know the device type, ship it every driver's wakeup
     * in hopes it will respond.  But not to USB or Bluetooth, because
     * shipping probe strings to unknown USB serial adaptors or
     * Bluetooth devices may spam devices that aren't GPSes at all and
     * could become confused.
     * For now we probe SOURCE_ACM...
     */
    if (!session->context->readonly &&
        SOURCE_USB != session->sourcetype &&
        SOURCE_BLUETOOTH != session->sourcetype) {

        if (0 != isatty(session->gpsdata.gps_fd)
            && !session->context->readonly) {
            if (NULL == session->device_type) {
                const struct gps_type_t **dp;
                for (dp = gpsd_drivers; *dp; dp++)
                    if (NULL != (*dp)->event_hook)
                        (*dp)->event_hook(session, event_wakeup);
            } else if (NULL != session->device_type->event_hook) {
                session->device_type->event_hook(session, event_wakeup);
            }
        }
    }
    packet_reset(&session->lexer);
    clock_gettime(CLOCK_REALTIME, &session->ts_startCurrentBaud);
}

/* open a device for access to its data
 * return: the opened file descriptor
 *         PLACEHOLDING_FD - for /dev/ppsX
 *         UNALLOCATED_FD - for open failure
 */
int gpsd_serial_open(struct gps_device_t *session)
{
    speed_t new_speed;
    char new_parity;   // E, N, O
    unsigned int new_stop;

    mode_t mode = (mode_t) O_RDWR;

    session->sourcetype = gpsd_classify(session);
    session->servicetype = service_sensor;

    if (SOURCE_UNKNOWN == session->sourcetype) {
        return UNALLOCATED_FD;
    }

    // we may need to hold on to this slot without opening the device
    if (SOURCE_PPS == session->sourcetype) {
        (void)gpsd_switch_driver(session, "PPS");
        return PLACEHOLDING_FD;
    }

    if (session->context->readonly ||
        (SOURCE_BLOCKDEV >= session->sourcetype)) {
        mode = (mode_t) O_RDONLY;
        GPSD_LOG(LOG_INF, &session->context->errout,
                 "SER: opening read-only GPS data source type %d at '%s'\n",
                 (int)session->sourcetype, session->gpsdata.dev.path);
    } else {
        GPSD_LOG(LOG_INF, &session->context->errout,
                 "SER: opening GPS data source type %d at '%s'\n",
                 (int)session->sourcetype, session->gpsdata.dev.path);
    }
#ifdef ENABLE_BLUEZ
    if (0 == bachk(session->gpsdata.dev.path)) {
        struct sockaddr_rc addr = { 0, *BDADDR_ANY, 0};

        errno = 0;
        session->gpsdata.gps_fd = socket(AF_BLUETOOTH,
                                         SOCK_STREAM,
                                         BTPROTO_RFCOMM);
        if (0 > session->gpsdata.gps_fd) {
            GPSD_LOG(LOG_ERROR, &session->context->errout,
                     "SER: bluetooth socket() failed: %s(%d)\n",
                     strerror(errno), errno);
            return UNALLOCATED_FD;
        }
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_channel = (uint8_t) 1;
        (void) str2ba(session->gpsdata.dev.path, &addr.rc_bdaddr);
        if (-1 == connect(session->gpsdata.gps_fd,
                          (struct sockaddr *) &addr,
                          sizeof (addr))) {
            if (EINPROGRESS != errno && EAGAIN != errno) {
                (void)close(session->gpsdata.gps_fd);
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: bluetooth socket connect failed: %s(%d)\n",
                         strerror(errno), errno);
                return UNALLOCATED_FD;
            }
            GPSD_LOG(LOG_ERROR, &session->context->errout,
                     "SER: bluetooth socket connect in progress or "
                     "EAGAIN: %s(%d)\n",
                     strerror(errno), errno);
        }
        (void)fcntl(session->gpsdata.gps_fd, F_SETFL, (int)mode);
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "SER: bluez device open success: %s %s(%d)\n",
                 session->gpsdata.dev.path, strerror(errno), errno);
    } else
#endif   // BLUEZ
    {
        /*
         * We open with O_NONBLOCK because we want to not get hung if
         * the CLOCAL flag is off.  Need to keep O_NONBLOCK so the main
         * loop does not clock on an unresponsive read() from a receiver.
         */
        errno = 0;
        if (-1 == (session->gpsdata.gps_fd =
                   open(session->gpsdata.dev.path,
                        (int)(mode | O_NONBLOCK | O_NOCTTY)))) {
            GPSD_LOG(LOG_ERROR, &session->context->errout,
                     "SER: device open of %s failed: %s(%d) - "
                     "retrying read-only\n",
                     session->gpsdata.dev.path,
                     strerror(errno), errno);
            if (-1 == (session->gpsdata.gps_fd =
                       open(session->gpsdata.dev.path,
                            O_RDONLY | O_NONBLOCK | O_NOCTTY))) {
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: read-only device open of %s failed: %s(%d)\n",
                         session->gpsdata.dev.path,
                         strerror(errno), errno);
                return UNALLOCATED_FD;
            }

            GPSD_LOG(LOG_PROG, &session->context->errout,
                     "SER: file device open of %s succeeded\n",
                     session->gpsdata.dev.path);
        }
    }

    /*
     * Ideally we want to exclusion-lock the device before doing any reads.
     * It would have been best to do this at open(2) time, but O_EXCL
     * doesn't work without O_CREAT.
     *
     * We have to make an exception for ptys, which are intentionally
     * opened by another process on the master side, otherwise we'll
     * break all our regression tests.
     *
     * We also exclude bluetooth device because the bluetooth daemon opens them.
     */
    if (!(SOURCE_PTY == session->sourcetype ||
          SOURCE_BLUETOOTH == session->sourcetype)) {
#ifdef TIOCEXCL
        /*
         * Try to block other processes from using this device while we
         * have it open (later opens should return EBUSY).  Won't work
         * against anything with root privileges, alas.
         */
        (void)ioctl(session->gpsdata.gps_fd, (unsigned long)TIOCEXCL);
#endif /* TIOCEXCL */

#ifdef __linux__
        /*
         * Don't touch devices already opened by another process.
         */
        if (1 < fusercount(session)) {
            GPSD_LOG(LOG_ERROR, &session->context->errout,
                     "SER: %s already opened by another process\n",
                     session->gpsdata.dev.path);
            (void)close(session->gpsdata.gps_fd);
            session->gpsdata.gps_fd = UNALLOCATED_FD;
            return UNALLOCATED_FD;
        }
#endif   // __linux__
    }

    session->lexer.type = BAD_PACKET;

    if (0 == isatty(session->gpsdata.gps_fd)) {
        GPSD_LOG(LOG_IO, &session->context->errout,
                 "SER: gpsd_serial_open(%s) -> %d, Not tty\n",
                 session->gpsdata.dev.path, session->gpsdata.gps_fd);
        return session->gpsdata.gps_fd;
    }

    // Save original terminal parameters, why?
    //  At least it tests we can read port parameters.
    if (0 != tcgetattr(session->gpsdata.gps_fd, &session->ttyset_old)) {
        // Maybe still useable somehow?
        return UNALLOCATED_FD;
    }
    session->ttyset = session->ttyset_old;

    if (0 < session->context->fixed_port_speed) {
        session->saved_baud = session->context->fixed_port_speed;
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "SER: fixed speed %d\n", session->saved_baud);
    }

    if (-1 != session->saved_baud) {
        (void)cfsetispeed(&session->ttyset, (speed_t)session->saved_baud);
        (void)cfsetospeed(&session->ttyset, (speed_t)session->saved_baud);
        if (0 == tcsetattr(session->gpsdata.gps_fd, TCSANOW,
                           &session->ttyset)) {
            GPSD_LOG(LOG_PROG, &session->context->errout,
                     "SER: set speed %d(%d)\n", session->saved_baud,
                     cfgetispeed(&session->ttyset));
        } else {
            GPSD_LOG(LOG_ERROR, &session->context->errout,
                     "SER: Error setting port attributes: %s(%d)\n",
                     strerror(errno), errno);
        }
        (void)tcflush(session->gpsdata.gps_fd, TCIOFLUSH);
    }

    // twiddle the speed, parity, etc. but only on real serial ports
    memset(session->ttyset.c_cc, 0, sizeof(session->ttyset.c_cc));
    //session->ttyset.c_cc[VTIME] = 1;
    /*
     * Tip from Chris Kuethe: the FIDI chip used in the Trip-Nav
     * 200 (and possibly other USB GPSes) gets completely hosed
     * in the presence of flow control.  Thus, turn off CRTSCTS.
     *
     * This is not ideal.  Setting no parity here will mean extra
     * initialization time for some devices, like certain Trimble
     * boards, that want 7O2 or other non-8N1 settings. But starting
     * the hunt loop at 8N1 will minimize the average sync time
     * over all devices.
     */
    session->ttyset.c_cflag &= ~(PARENB | PARODD | CRTSCTS | CSTOPB);
    session->ttyset.c_cflag |= CREAD | CLOCAL;
    session->ttyset.c_iflag = session->ttyset.c_oflag =
        session->ttyset.c_lflag = (tcflag_t) 0;

    session->baudindex = 0;  // FIXME: fixed speed
    if (0 < session->context->fixed_port_speed) {
        new_speed = session->context->fixed_port_speed;
    } else {
        new_speed = gpsd_get_speed_old(session);
    }
    if ('\0' == session->context->fixed_port_framing[0]) {
        new_parity = 'N';
        new_stop = 1;
    } else {
        // ignore length, stopbits=2 forces length 7.
        new_parity = session->context->fixed_port_framing[1];
        new_stop = session->context->fixed_port_framing[2] - '0';
    }
    // FIXME: setting speed twice??
    gpsd_set_speed(session, new_speed, new_parity, new_stop);

    /* Used to turn off O_NONBLOCK here, but best not to block trying
     * to read from an unresponsive receiver. */

    // required so parity field won't be '\0' if saved speed matches
    if (SOURCE_BLOCKDEV >= session->sourcetype) {
        session->gpsdata.dev.parity = 'N';
        session->gpsdata.dev.stopbits = 1;
    }

    // start the autobaud hunt clock.
    clock_gettime(CLOCK_REALTIME, &session->ts_startCurrentBaud);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "SER: open(%s) -> %d in gpsd_serial_open()\n",
             session->gpsdata.dev.path, session->gpsdata.gps_fd);
    return session->gpsdata.gps_fd;
}

ssize_t gpsd_serial_write(struct gps_device_t * session,
                          const char *buf, const size_t len)
{
    ssize_t status;
    bool ok;
    char scratchbuf[MAX_PACKET_LENGTH*2+1];

    if (NULL == session ||
        NULL == session->context ||
        session->context->readonly)
        return 0;

    status = write(session->gpsdata.gps_fd, buf, len);
    ok = (status == (ssize_t) len);
    (void)tcdrain(session->gpsdata.gps_fd);

    GPSD_LOG(LOG_IO, &session->context->errout,
             "SER: => GPS: %s%s\n",
             gpsd_packetdump(scratchbuf, sizeof(scratchbuf),
                             (char *)buf, len), ok ? "" : " FAILED");
    return status;
}

/*
 * This constant controls how many characters the packet sniffer will spend
 * looking for a packet leader before it gives up.  It *must* be larger than
 * MAX_PACKET_LENGTH or we risk never syncing up at all.  Large values
 * will produce annoying startup lag.
 */
#define SNIFF_RETRIES   (MAX_PACKET_LENGTH + 128)

// advance to the next hunt setting
bool gpsd_next_hunt_setting(struct gps_device_t * session)
{
    struct timespec ts_now, ts_diff;

    // every rate we're likely to see on an GNSS receiver

    // don't waste time in the hunt loop if this is not actually a tty
    // FIXME: Check for ttys like /dev/ttyACM that have no speed.
    if (0 == isatty(session->gpsdata.gps_fd))
        return false;

    // ...or if it's nominally a tty but delivers only PPS and no data
    if (SOURCE_PPS == session->sourcetype) {
        return false;
    }

    clock_gettime(CLOCK_REALTIME, &ts_now);
         TS_SUB(&ts_diff, &session->ts_startCurrentBaud, &ts_now);

    GPSD_LOG(LOG_IO, &session->context->errout,
             "SER: gpsd_next_hunt_setting(%d) retries %lu start %lld\n",
             session->gpsdata.gps_fd,
             session->lexer.retry_counter,
             (long long)session->ts_startCurrentBaud.tv_sec);
    if (SNIFF_RETRIES <= session->lexer.retry_counter++ ||
        3 < ts_diff.tv_sec) {
        // no lock after 3 seconds or SNIFF_RETRIES
        char new_parity;   // E, N, O
        unsigned int new_stop;
        // u-blox 9 can do 921600
        // Javad can ro 1.5 mbps
        static unsigned int rates[] =
            {0, 4800, 9600, 19200, 38400, 57600, 115200, 230400,
             460800, 921600};

        if (0 < session->context->fixed_port_speed) {
            //  fixed speed, don't hunt
            //  this prevents framing hunt?
            return false;
        }

        if ((unsigned int)((sizeof(rates) / sizeof(rates[0])) - 1) <=
            session->baudindex++) {

            session->baudindex = 0;
            if ('\0' != session->context->fixed_port_framing[0]) {
                return false;   /* hunt is over, no sync */
            }

            // More stop bits to try?
            if (2 <= session->gpsdata.dev.stopbits++) {
                return false;   /* hunt is over, no sync */
            }
        }

        if ('\0' == session->context->fixed_port_framing[0]) {
            new_parity = session->gpsdata.dev.parity;
            new_stop = session->gpsdata.dev.stopbits;
        } else {
            // ignore length, stopbits=2 forces length 7.
            new_parity = session->context->fixed_port_framing[1];
            new_stop = session->context->fixed_port_framing[2] - '0';
        }

        gpsd_set_speed(session, rates[session->baudindex], new_parity,
                       new_stop);
        session->lexer.retry_counter = 0;
    }
    return true;                // keep hunting
}

// to be called when we want to register that we've synced with a device
void gpsd_assert_sync(struct gps_device_t *session)
{
    /*
     * We've achieved first sync with the device. Remember the
     * baudrate so we can try it first next time this device
     * is opened.
     */
    if (-1 == session->saved_baud)
        session->saved_baud = (int)cfgetispeed(&session->ttyset);
}

void gpsd_close(struct gps_device_t *session)
{
    if (!BAD_SOCKET(session->gpsdata.gps_fd)) {
#ifdef TIOCNXCL
        (void)ioctl(session->gpsdata.gps_fd, (unsigned long)TIOCNXCL);
#endif  // TIOCNXCL
        if (!session->context->readonly) {
            // Be sure all output is sent.
            if (0 != tcdrain(session->gpsdata.gps_fd)) {
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: gpsd_close() tcdrain() failed: %s(%d)\n",
                         strerror(errno), errno);
            }
        }

        if (0 != isatty(session->gpsdata.gps_fd)) {
            // Save current terminal parameters.  Why?
            if (0 != tcgetattr(session->gpsdata.gps_fd, &session->ttyset_old)) {
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: gpsd_close() tcgetattr() failed: %s(%d)\n",
                         strerror(errno), errno);
            }

            // force hangup on close on systems that don't do HUPCL properly
            // is this still an issue?
            (void)cfsetispeed(&session->ttyset, (speed_t)B0);
            (void)cfsetospeed(&session->ttyset, (speed_t)B0);
            if (0 != tcsetattr(session->gpsdata.gps_fd, TCSANOW,
                               &session->ttyset)) {
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: tcsetattr(B0) failed: %s(%d)\n",
                         strerror(errno), errno);
            }

            // this is the clean way to do it
            session->ttyset_old.c_cflag |= HUPCL;
            if (0 != tcsetattr(session->gpsdata.gps_fd, TCSANOW,
                               &session->ttyset_old)) {
                GPSD_LOG(LOG_ERROR, &session->context->errout,
                         "SER: tcsetattr(%d) failed: %s(%d)\n",
                         session->gpsdata.dev.baudrate, strerror(errno),
                         errno);
            }
        }
        GPSD_LOG(LOG_IO, &session->context->errout,
                 "SER: gpsd_close(%s), close(%d)\n",
                 session->gpsdata.dev.path,
                 session->gpsdata.gps_fd);
        (void)close(session->gpsdata.gps_fd);
        session->gpsdata.gps_fd = -1;
    }
}
// vim: set expandtab shiftwidth=4
