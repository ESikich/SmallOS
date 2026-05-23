#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "wolf3d_port.h"

void wolf3d_upstream_main(void);
void CheckForEpisodes(void);
void Patch386(void);
void InitGame(void);
void DemoLoop(void);
void NewGame(int difficulty, int episode);
void GameLoop(void);
void US_ControlPanel(unsigned char scancode);
void ShutdownId(void);
extern char extension[5];

extern int wolf3d_probe_stop_after_demo_prelude;
extern int wolf3d_probe_stop_after_title_frame;
extern int wolf3d_probe_stop_after_control_panel_frame;
extern int wolf3d_probe_stop_after_first_game_frame;

static void use_nowait_startup_args(void) {
    static char arg0[] = "wolf3d-srcprobe";
    static char arg1[] = "NOWAIT";
    static char* argv[] = {arg0, arg1, NULL};

    wolf3d_argc = 2;
    wolf3d_argv = argv;
}

/*
 * Separate executable for the source-faithful port track. It deliberately
 * does not replace src/user/wolf3d.c yet; it proves that the generated mirror
 * and SmallOS shims can coexist in a normal user ELF.
 */
int main(int argc, char** argv) {
    wolf3d_argc = argc;
    wolf3d_argv = argv;

    if (argc > 1 && strcmp(argv[1], "--upstream-main") == 0) {
        (void)chdir("/usr/share/wolf3d");
        wolf3d_upstream_main();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--check-episodes") == 0) {
        (void)chdir("/usr/share/wolf3d");
        CheckForEpisodes();
        Patch386();
        printf("wolf3d-source-probe: upstream data extension %s\n", extension);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--init-game") == 0) {
        (void)chdir("/usr/share/wolf3d");
        use_nowait_startup_args();
        CheckForEpisodes();
        Patch386();
        InitGame();
        ShutdownId();
        printf("wolf3d-source-probe: upstream InitGame completed %s\n",
               extension);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--demo-preamble") == 0) {
        (void)chdir("/usr/share/wolf3d");
        use_nowait_startup_args();
        CheckForEpisodes();
        Patch386();
        InitGame();
        wolf3d_probe_stop_after_demo_prelude = 1;
        DemoLoop();
        wolf3d_probe_stop_after_demo_prelude = 0;
        ShutdownId();
        printf("wolf3d-source-probe: upstream DemoLoop prelude completed %s\n",
               extension);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--title-frame") == 0) {
        (void)chdir("/usr/share/wolf3d");
        use_nowait_startup_args();
        CheckForEpisodes();
        Patch386();
        InitGame();
        wolf3d_probe_stop_after_title_frame = 1;
        DemoLoop();
        wolf3d_probe_stop_after_title_frame = 0;
        ShutdownId();
        printf("wolf3d-source-probe: upstream DemoLoop title frame completed %s\n",
               extension);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--control-panel-frame") == 0) {
        (void)chdir("/usr/share/wolf3d");
        use_nowait_startup_args();
        CheckForEpisodes();
        Patch386();
        InitGame();
        wolf3d_probe_stop_after_control_panel_frame = 1;
        US_ControlPanel(0);
        wolf3d_probe_stop_after_control_panel_frame = 0;
        ShutdownId();
        printf("wolf3d-source-probe: upstream control panel frame completed %s\n",
               extension);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--first-game-frame") == 0) {
        (void)chdir("/usr/share/wolf3d");
        use_nowait_startup_args();
        CheckForEpisodes();
        Patch386();
        InitGame();
        NewGame(1, 0);
        wolf3d_probe_stop_after_first_game_frame = 1;
        GameLoop();
        wolf3d_probe_stop_after_first_game_frame = 0;
        ShutdownId();
        printf("wolf3d-source-probe: upstream first game frame completed %s\n",
               extension);
        return 0;
    }

    puts("wolf3d-source-probe: upstream startup path linked");
    return 0;
}
