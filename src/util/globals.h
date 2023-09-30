#ifndef ARDOS_GLOBALS_H
#define ARDOS_GLOBALS_H

#include <memory>
#include <thread>

#include <dcFile.h>
#include <uvw/loop.h>

namespace Ardos {

/**
 * Some helpful global variables that are accessed frequently.
 * For stateful classes, prefer singleton pattern.
 */

extern DCFile *g_dc_file;
extern std::thread::id g_main_thread_id;
extern std::shared_ptr<uvw::loop> g_loop;

typedef std::map<const DCField *, std::vector<uint8_t>> FieldMap;

} // namespace Ardos

#endif // ARDOS_GLOBALS_H
