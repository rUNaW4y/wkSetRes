#ifndef WKSETRES_DEBUGF_H
#define WKSETRES_DEBUGF_H

#define debugf(fmt, ...) printf("%s:%d: " fmt, __FUNCTION__, __LINE__, __VA_ARGS__)

#endif // WKSETRES_DEBUGF_H
