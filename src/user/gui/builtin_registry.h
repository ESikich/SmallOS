#ifndef SMALLOS_GUI_BUILTIN_REGISTRY_H
#define SMALLOS_GUI_BUILTIN_REGISTRY_H

#include "framework.h"

unsigned int gui_builtin_descriptor_count(void);
const gui_app_descriptor_t* gui_builtin_descriptor_at(unsigned int index);

#endif
