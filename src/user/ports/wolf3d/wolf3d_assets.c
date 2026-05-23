/*
 * Storage for original Wolf3D data blobs that the Borland sources address as
 * linker-provided symbols. The platform shim copies generated extracted data
 * into these buffers during startup.
 */

char signon[64000];
unsigned char gamepal[768];
