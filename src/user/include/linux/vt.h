#ifndef _LINUX_VT_H
#define _LINUX_VT_H

/*
 * SmallOS has one console/PTY model rather than Linux virtual terminals.
 * Keep this header intentionally small so BusyBox code can include it while
 * VT-specific code remains disabled by the absence of VT_* request macros.
 */

#endif
