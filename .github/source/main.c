#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    fatInitDefault();

    if (argc < 2 || argv[1] == NULL) {
        // 경로가 없으면 그냥 종료
        return 1;
    }

    const char *romPath = argv[1];

    // .sav 경로 생성 (확장자 .nds → .sav)
    char savPath[512];
    strncpy(savPath, romPath, sizeof(savPath) - 1);
    savPath[sizeof(savPath) - 1] = '\0';
    char *ext = strrchr(savPath, '.');
    if (ext) strcpy(ext, ".sav");

    // nds-bootstrap.ini 작성
    FILE *f = fopen("fat:/_nds/nds-bootstrap.ini", "w");
    if (!f) return 1;

    fprintf(f, "[NDS-BOOTSTRAP]\n");
    fprintf(f, "NDS_PATH = %s\n", romPath);
    fprintf(f, "SAV_PATH = %s\n", savPath);
    fprintf(f, "BOOTSTRAP_PATH = fat:/_nds/nds-bootstrap-release.nds\n");
    fprintf(f, "BOOST_CPU = 1\n");
    fprintf(f, "CARD_READ_DMA = 1\n");
    fprintf(f, "ASYNC_CARD_READ = 1\n");
    fprintf(f, "DSI_MODE = 0\n");
    fclose(f);

    // nds-bootstrap 실행
    const char *bsPath = "fat:/_nds/nds-bootstrap-release.nds";
    char *bsArgs[] = { (char *)bsPath, NULL };
    execv(bsPath, bsArgs);

    // execv 실패 시
    return 1;
}