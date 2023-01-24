#include "globals.h"

namespace Ardos {

DCFile *g_dc_file = nullptr;
std::thread::id g_main_thread_id;
std::shared_ptr<uvw::Loop> g_loop;

} // namespace Ardos