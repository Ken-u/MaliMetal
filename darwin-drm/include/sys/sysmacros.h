/* Darwin 的 <sys/types.h> 自带 major()/minor()/makedev()，没有 <sys/sysmacros.h>；PanVK 只是 include 它。 */
#include <sys/types.h>
