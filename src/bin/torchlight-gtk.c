/* Thin GTK executable: own the launcher application and its exit status. */
#include "torchlight/launcher.h"
int main(int argc, char **argv) {
    tl_launcher *launcher = NULL;
    tl_status status = launcher_create(&launcher);
    int exit_code = 1;
    if (status == TL_OK)
        status = launcher_run(launcher, argc, argv, &exit_code);
    launcher_destroy(launcher);
    return status == TL_OK ? exit_code : 1;
}
