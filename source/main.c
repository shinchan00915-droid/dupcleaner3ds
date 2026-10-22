// 3DS 중복 게임 정리기 (Duplicate Game Cleaner)
// 설치된 게임(SD)을 전부 스캔 -> 같은 게임의 지역판(일/한/영/유럽..)을 묶어서 보여주고
// 고른 것만 게임 + 세이브 + 티켓을 삭제하는 홈브류.
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------- SMDH
typedef struct { u16 shortDesc[0x40]; u16 longDesc[0x80]; u16 publisher[0x40]; } SMDHTitle;
typedef struct {
    char magic[4]; u16 version; u16 reserved;
    SMDHTitle titles[16];
    u8 settings[0x30]; u8 reserved2[8];
    u8 smallIcon[0x480];
    u16 bigIcon[0x900];
} SMDH;

// ---------------------------------------------------------------- 데이터
typedef struct {
    u64 id, size;
    char code[16];     // 예: CTR-P-AMKJ
    char key[12];      // 같은 게임 판별 키 (게임코드 가운데 2글자)
    char region;       // 게임코드 마지막 글자
    char name[160];    // 화면에 표시할 이름 (잘라낸 것)
    bool hangul, patched, useKR, hasIcon, mark, deleted;
    C3D_Tex tex;
    C2D_Image img;
} Title;

static Title* g_titles = NULL;
static int    g_count  = 0;
static int*   g_view   = NULL;   // 화면에 보이는 목록 (g_titles 인덱스)
static int    g_viewN  = 0;
static int*   g_gsize  = NULL;   // 각 title의 그룹 크기
static int*   g_gid    = NULL;   // 각 title의 그룹 번호
static bool   g_showAll = false;
static bool   g_delSave = true;

static C3D_RenderTarget *g_top, *g_bot;
static C2D_TextBuf g_buf, g_tmp;
static C2D_Font g_fontKR = NULL;
static bool g_korUi = false;

static const Tex3DS_SubTexture g_subt = {48, 48, 0.0f, 48 / 64.0f, 48 / 64.0f, 0.0f};

#define T(ko, en) (g_korUi ? (ko) : (en))
#define COL(r, g, b) C2D_Color32((r), (g), (b), 255)

// ---------------------------------------------------------------- 텍스트
static void drawText(C2D_Font font, const char* s, float x, float y, float sc, u32 color, u32 flags)
{
    C2D_Text t;
    C2D_TextFontParse(&t, font, g_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, sc, sc, color);
}
static void ui(const char* s, float x, float y, float sc, u32 color, u32 flags)
{
    drawText(g_korUi ? g_fontKR : NULL, s, x, y, sc, color, flags);
}

static float textWidth(C2D_Font font, const char* s, float sc)
{
    C2D_TextBufClear(g_tmp);
    C2D_Text t; float w = 0, h = 0;
    C2D_TextFontParse(&t, font, g_tmp, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}

// maxW 픽셀 안에 들어오도록 UTF-8 문자열 뒤를 자름
static void fitText(char* s, C2D_Font font, float sc, float maxW)
{
    if (textWidth(font, s, sc) <= maxW) return;
    size_t len = strlen(s);
    while (len > 1) {
        len--;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;   // 문자 경계
        s[len] = 0;
        char tmp[200]; snprintf(tmp, sizeof tmp, "%s...", s);
        if (textWidth(font, tmp, sc) <= maxW) { strcpy(s, tmp); return; }
    }
}

// ---------------------------------------------------------------- 지역
static const char* regionName(char r)
{
    switch (r) {
        case 'J': return T("일본판", "Japan");
        case 'E': return T("북미판", "USA");
        case 'P': return T("유럽판", "Europe");
        case 'K': return T("한국판", "Korea");
        case 'C': return T("중국판", "China");
        case 'T': return T("대만판", "Taiwan");
        case 'A': return T("전지역", "All");
        case 'D': return T("유럽(독일)", "EUR-DE");
        case 'F': return T("유럽(프랑스)", "EUR-FR");
        case 'I': return T("유럽(이탈리아)", "EUR-IT");
        case 'S': return T("유럽(스페인)", "EUR-ES");
        case 'H': return T("유럽(네덜란드)", "EUR-NL");
        case 'R': return T("유럽(러시아)", "EUR-RU");
        case 'U': return T("호주판", "Australia");
        case 'X': case 'Y': case 'Z': return T("유럽판", "Europe");
        default:  return T("알수없음", "Unknown");
    }
}
static u32 regionColor(char r)
{
    switch (r) {
        case 'K': return COL(90, 220, 130);
        case 'J': return COL(255, 120, 120);
        case 'E': return COL(110, 170, 255);
        case 'P': case 'D': case 'F': case 'I': case 'S': case 'H': case 'R': case 'X': case 'Y': case 'Z':
                  return COL(255, 205, 90);
        default:  return COL(190, 190, 190);
    }
}
static int regionOrder(char r)
{
    switch (r) { case 'K': return 0; case 'J': return 1; case 'E': return 2; case 'P': return 3; default: return 4; }
}
static int langForRegion(char r)
{
    switch (r) {
        case 'J': return 0; case 'K': return 7; case 'C': return 6; case 'T': return 11;
        case 'F': return 2; case 'D': return 3; case 'I': return 4; case 'S': return 5;
        case 'H': return 8; case 'R': return 10; default: return 1;
    }
}

// ---------------------------------------------------------------- SMDH 읽기
static bool loadSMDH(u64 id, SMDH* out)
{
    u32 archivePath[] = {(u32)(id & 0xFFFFFFFF), (u32)(id >> 32), MEDIATYPE_SD, 0x00000000};
    FS_Path apath = {PATH_BINARY, 0x10, archivePath};
    u32 filePath[] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};   // ExeFS "icon"
    FS_Path fpath = {PATH_BINARY, 0x14, filePath};
    Handle fh;
    if (R_FAILED(FSUSER_OpenFileDirectly(&fh, ARCHIVE_SAVEDATA_AND_CONTENT, apath, fpath, FS_OPEN_READ, 0)))
        return false;
    u32 rd = 0;
    Result r = FSFILE_Read(fh, &rd, 0, out, sizeof(SMDH));
    FSFILE_Close(fh);
    return R_SUCCEEDED(r) && rd >= sizeof(SMDH) && memcmp(out->magic, "SMDH", 4) == 0;
}

static bool hasHangul16(const u16* s, int n)
{
    for (int i = 0; i < n && s[i]; i++)
        if ((s[i] >= 0xAC00 && s[i] <= 0xD7A3) || (s[i] >= 0x1100 && s[i] <= 0x11FF) || (s[i] >= 0x3130 && s[i] <= 0x318F))
            return true;
    return false;
}

static void makeIcon(Title* t, const SMDH* smdh)
{
    if (!C3D_TexInit(&t->tex, 64, 64, GPU_RGB565)) return;
    memset(t->tex.data, 0, 64 * 64 * 2);
    u16* dest = (u16*)t->tex.data + (64 - 48) * 64;
    const u16* src = smdh->bigIcon;
    for (int j = 0; j < 48; j += 8) {
        memcpy(dest, src, 48 * 8 * sizeof(u16));
        src += 48 * 8;
        dest += 64 * 8;
    }
    GSPGPU_FlushDataCache(t->tex.data, 64 * 64 * 2);
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    t->img.tex = &t->tex;
    t->img.subtex = &g_subt;
    t->hasIcon = true;
}

// ---------------------------------------------------------------- 진행 표시
static void progress(const char* msg, int cur, int total)
{
    C2D_TextBufClear(g_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(g_top, COL(24, 24, 32));
    C2D_TargetClear(g_bot, COL(24, 24, 32));
    C2D_SceneBegin(g_top);
    ui(msg, 20, 90, 0.7f, COL(255, 255, 255), 0);
    char b[64]; snprintf(b, sizeof b, "%d / %d", cur, total);
    ui(b, 20, 125, 0.7f, COL(180, 200, 255), 0);
    float w = total > 0 ? 360.0f * cur / total : 0;
    C2D_DrawRectSolid(20, 165, 0.5f, 360, 14, COL(50, 50, 70));
    C2D_DrawRectSolid(20, 165, 0.5f, w, 14, COL(90, 200, 130));
    C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- 스캔
static int cmpTitle(const void* a, const void* b)
{
    const Title* x = a; const Title* y = b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    c = regionOrder(x->region) - regionOrder(y->region);
    if (c) return c;
    return (x->id < y->id) ? -1 : (x->id > y->id);
}

static void rebuildView(void)
{
    int gid = 0;
    // key가 같은 연속 구간 = 같은 게임 (정렬되어 있음)
    int i = 0;
    while (i < g_count) {
        int j = i, n = 0;
        while (j < g_count && strcmp(g_titles[j].key, g_titles[i].key) == 0) { if (!g_titles[j].deleted) n++; j++; }
        for (int k = i; k < j; k++) { g_gsize[k] = n; g_gid[k] = g_titles[k].deleted ? -1 : gid; }
        if (n > 0) gid++;
        i = j;
    }
    g_viewN = 0;
    for (int k = 0; k < g_count; k++) {
        if (g_titles[k].deleted) continue;
        if (g_showAll || g_gsize[k] >= 2) g_view[g_viewN++] = k;
    }
}

static void scanTitles(void)
{
    u32 count = 0;
    AM_GetTitleCount(MEDIATYPE_SD, &count);
    if (count == 0) return;
    u64* ids = malloc(count * sizeof(u64));
    u32 read = 0;
    AM_GetTitleList(&read, MEDIATYPE_SD, count, ids);
    u32 apps = 0;
    for (u32 i = 0; i < read; i++)
        if ((ids[i] >> 32) == 0x00040000ULL) ids[apps++] = ids[i];   // 게임(앱)만
    if (apps == 0) { free(ids); return; }

    AM_TitleInfo* infos = calloc(apps, sizeof(AM_TitleInfo));
    AM_GetTitleInfo(MEDIATYPE_SD, apps, ids, infos);

    g_titles = calloc(apps, sizeof(Title));
    g_gsize = calloc(apps, sizeof(int));
    g_gid = calloc(apps, sizeof(int));
    g_view = calloc(apps, sizeof(int));
    g_count = (int)apps;

    SMDH* smdh = malloc(sizeof(SMDH));
    for (u32 i = 0; i < apps; i++) {
        if (i % 4 == 0) progress(T("게임 스캔 중...", "Scanning games..."), (int)i, (int)apps);
        Title* t = &g_titles[i];
        t->id = ids[i];
        t->size = infos[i].size;

        char pc[16] = {0};
        if (R_FAILED(AM_GetTitleProductCode(MEDIATYPE_SD, t->id, pc)) || strlen(pc) < 10) snprintf(pc, sizeof pc, "CTR-P-????");
        strncpy(t->code, pc, 15);
        t->region = pc[9];
        if (pc[6] == '?' || strstr(pc, "????")) snprintf(t->key, sizeof t->key, "%016llX", (unsigned long long)t->id);   // 모르면 단독 취급
        else { t->key[0] = pc[7]; t->key[1] = pc[8]; t->key[2] = 0; }

        char name[160] = {0};
        bool ok = loadSMDH(t->id, smdh);
        if (ok) {
            int slot = -1;
            if (hasHangul16(smdh->titles[7].shortDesc, 0x40)) slot = 7;
            else for (int l = 0; l < 12; l++) if (hasHangul16(smdh->titles[l].shortDesc, 0x40)) { slot = l; break; }
            t->hangul = (slot >= 0);
            if (slot < 0) {
                slot = langForRegion(t->region);
                if (!smdh->titles[slot].shortDesc[0]) {
                    int pref[] = {1, 0, 7, 2, 3, 4, 5, 6, 8, 9, 10, 11};
                    for (int p = 0; p < 12; p++) if (smdh->titles[pref[p]].shortDesc[0]) { slot = pref[p]; break; }
                }
            }
            if (smdh->titles[slot].shortDesc[0])
                utf16_to_utf8((uint8_t*)name, smdh->titles[slot].shortDesc, sizeof(name) - 1);
            for (char* c = name; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            makeIcon(t, smdh);
        }
        t->patched = t->hangul && t->region != 'K';
        t->useKR = t->hangul && g_fontKR;
        if (!name[0]) snprintf(name, sizeof name, "%s", t->code);
        strncpy(t->name, name, sizeof(t->name) - 1);
        fitText(t->name, t->useKR ? g_fontKR : NULL, 0.5f, 285.0f);
    }
    free(smdh); free(infos); free(ids);

    qsort(g_titles, g_count, sizeof(Title), cmpTitle);
    // qsort로 위치가 바뀌었으므로 img.tex 포인터 재설정 (C3D_Tex는 구조체 안에 있음)
    for (int i = 0; i < g_count; i++) if (g_titles[i].hasIcon) g_titles[i].img.tex = &g_titles[i].tex;
    rebuildView();
}

// ---------------------------------------------------------------- 삭제
static void rmrf(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { remove(path); return; }
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[512]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            rmrf(p);
        }
        closedir(d);
    }
    rmdir(path);
}

// sdmc:/Nintendo 3DS/<id0>/<id1>/title/<hi>/<lo> 폴더 삭제 (세이브 데이터 포함)
static void deleteSaveFolders(u64 id)
{
    const char* base = "sdmc:/Nintendo 3DS";
    DIR* d0 = opendir(base);
    if (!d0) return;
    struct dirent* e0;
    while ((e0 = readdir(d0))) {
        if (e0->d_name[0] == '.') continue;
        char p0[300]; snprintf(p0, sizeof p0, "%s/%s", base, e0->d_name);
        DIR* d1 = opendir(p0);
        if (!d1) continue;
        struct dirent* e1;
        while ((e1 = readdir(d1))) {
            if (e1->d_name[0] == '.') continue;
            char p[420];
            snprintf(p, sizeof p, "%s/%s/title/%08lx/%08lx", p0, e1->d_name, (unsigned long)(id >> 32), (unsigned long)(id & 0xFFFFFFFF));
            rmrf(p);
        }
        closedir(d1);
    }
    closedir(d0);
}

static int markedCount(void)
{
    int n = 0;
    for (int i = 0; i < g_count; i++) if (g_titles[i].mark && !g_titles[i].deleted) n++;
    return n;
}

static void doDelete(void)
{
    int total = markedCount(), done = 0, fails = 0;
    for (int i = 0; i < g_count; i++) {
        Title* t = &g_titles[i];
        if (!t->mark || t->deleted) continue;
        char msg[200]; snprintf(msg, sizeof msg, "%s %s", T("삭제 중:", "Deleting:"), t->code);
        progress(msg, done, total);
        Result r = AM_DeleteTitle(MEDIATYPE_SD, t->id);
        AM_DeleteTicket(t->id);   // 티켓은 없어도 상관없음
        if (g_delSave) deleteSaveFolders(t->id);
        if (R_FAILED(r)) fails++;
        else { t->deleted = true; t->mark = false; }
        done++;
    }
    progress(T("완료!", "Done!"), total, total);
    svcSleepThread(600000000LL);
    rebuildView();
    (void)fails;
}

// ---------------------------------------------------------------- 화면
static void drawMain(int sel, int top, bool confirm)
{
    C2D_TextBufClear(g_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(g_top, COL(22, 22, 30));
    C2D_TargetClear(g_bot, COL(22, 22, 30));

    // ---- 위 화면
    C2D_SceneBegin(g_top);
    C2D_DrawRectSolid(0, 0, 0.4f, 400, 24, COL(48, 52, 90));
    char head[160];
    int groups = 0, dupTitles = 0;
    for (int i = 0; i < g_count; i++) if (!g_titles[i].deleted && g_gsize[i] >= 2) dupTitles++;
    for (int i = 0; i < g_count; i++) if (!g_titles[i].deleted && g_gsize[i] >= 2 && (i == 0 || g_gid[i] != g_gid[i - 1])) groups++;
    snprintf(head, sizeof head, T("%s  중복 %d종 (%d개) / 전체 %d개", "%s  dup %d games (%d) / total %d"),
             g_showAll ? T("[전체]", "[ALL]") : T("[중복만]", "[DUPS]"), groups, dupTitles, g_count);
    ui(head, 8, 3, 0.55f, COL(255, 255, 255), 0);

    const int rowH = 42, y0 = 26, rows = 5;
    if (g_viewN == 0) {
        ui(T("중복된 게임이 없어요!  (Y: 전체 보기)", "No duplicates!  (Y: show all)"), 20, 100, 0.6f, COL(200, 220, 255), 0);
    }
    for (int r = 0; r < rows; r++) {
        int vi = top + r;
        if (vi >= g_viewN) break;
        int ti = g_view[vi];
        Title* t = &g_titles[ti];
        float y = y0 + r * rowH;
        bool alt = (g_gid[ti] % 2) == 0;
        u32 bg = (vi == sel) ? COL(70, 78, 140) : (alt ? COL(36, 36, 48) : COL(30, 30, 40));
        C2D_DrawRectSolid(2, y, 0.3f, 396, rowH - 2, bg);
        if (g_gsize[ti] >= 2) C2D_DrawRectSolid(2, y, 0.35f, 4, rowH - 2, regionColor(t->region));
        if (t->hasIcon) C2D_DrawImageAt(t->img, 9, y + 2, 0.5f, NULL, 0.8f, 0.8f);
        else C2D_DrawRectSolid(9, y + 2, 0.5f, 38, 38, COL(70, 70, 70));
        drawText(t->useKR ? g_fontKR : NULL, t->name, 54, y + 2, 0.5f, COL(255, 255, 255), 0);
        const char* rn = regionName(t->region);
        ui(rn, 54, y + 21, 0.45f, regionColor(t->region), 0);
        if (t->patched) ui(T("한글패치", "KR-patch"), 120, y + 21, 0.45f, COL(90, 220, 130), 0);
        char info[64]; snprintf(info, sizeof info, "%s  %lluMB", t->code + 6, (unsigned long long)(t->size / 1048576ULL));
        drawText(NULL, info, 390, y + 21, 0.45f, COL(150, 150, 170), C2D_AlignRight);
        if (t->mark) {
            C2D_DrawRectSolid(2, y, 0.6f, 396, rowH - 2, C2D_Color32(220, 40, 40, 110));
            ui("X", 372, y + 2, 0.7f, COL(255, 255, 255), 0);
        }
    }
    if (g_viewN > rows) {   // 스크롤바
        float h = 210.0f * rows / g_viewN; if (h < 10) h = 10;
        float yy = y0 + (210.0f - h) * top / (g_viewN - rows);
        C2D_DrawRectSolid(396, yy, 0.7f, 3, h, COL(120, 130, 200));
    }

    // ---- 아래 화면
    C2D_SceneBegin(g_bot);
    if (confirm) {
        char b[128]; snprintf(b, sizeof b, T("%d개를 삭제할까요?", "Delete %d title(s)?"), markedCount());
        ui(b, 20, 20, 0.7f, COL(255, 120, 120), 0);
        ui(T("게임 + 티켓", "Game + ticket"), 20, 60, 0.55f, COL(255, 255, 255), 0);
        ui(g_delSave ? T("+ 세이브 데이터 (복구 불가!)", "+ save data (can't undo!)") : T("세이브는 남겨둠", "save data is kept"), 20, 85, 0.55f,
           g_delSave ? COL(255, 160, 120) : COL(130, 230, 150), 0);
        ui(T("A: 삭제 실행    B: 취소", "A: delete   B: cancel"), 20, 150, 0.6f, COL(255, 255, 255), 0);
        ui(T("SELECT: 세이브 삭제 켜기/끄기", "SELECT: toggle save delete"), 20, 185, 0.5f, COL(180, 190, 220), 0);
    } else {
        if (g_viewN > 0) {
            Title* t = &g_titles[g_view[sel]];
            drawText(t->useKR ? g_fontKR : NULL, t->name, 10, 8, 0.55f, COL(255, 255, 255), 0);
            char b[160];
            snprintf(b, sizeof b, "%s  (%s)", t->code, regionName(t->region));
            ui(b, 10, 32, 0.5f, regionColor(t->region), 0);
            snprintf(b, sizeof b, "Title ID: %016llX", (unsigned long long)t->id);
            ui(b, 10, 52, 0.5f, COL(170, 170, 190), 0);
            snprintf(b, sizeof b, T("크기: %llu MB   그룹 내 %d개", "Size: %llu MB   %d in group"),
                     (unsigned long long)(t->size / 1048576ULL), g_gsize[g_view[sel]]);
            ui(b, 10, 72, 0.5f, COL(170, 170, 190), 0);
        }
        C2D_DrawRectSolid(8, 100, 0.4f, 304, 1, COL(70, 70, 100));
        ui(T("A: 삭제할 게임 선택/해제", "A: mark / unmark"), 10, 106, 0.5f, COL(255, 255, 255), 0);
        ui(T("X: 선택한 것 삭제", "X: delete marked"), 10, 126, 0.5f, COL(255, 150, 150), 0);
        ui(T("Y: 중복만 / 전체 보기", "Y: dups only / all"), 10, 146, 0.5f, COL(255, 255, 255), 0);
        ui(T("B: 선택 모두 해제    START: 종료", "B: clear marks   START: exit"), 10, 166, 0.5f, COL(255, 255, 255), 0);
        char b[96]; snprintf(b, sizeof b, T("삭제 선택: %d개   세이브 삭제: %s", "Marked: %d   Save delete: %s"),
                             markedCount(), g_delSave ? "ON" : "OFF");
        ui(b, 10, 200, 0.5f, g_delSave ? COL(255, 190, 120) : COL(130, 230, 150), 0);
    }
    C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- main
int main(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_buf = C2D_TextBufNew(8192);
    g_tmp = C2D_TextBufNew(512);
    amInit();
    fsInit();
    g_fontKR = C2D_FontLoadSystem(CFG_REGION_KOR);
    g_korUi = (g_fontKR != NULL);

    scanTitles();

    int sel = 0, top = 0, held = 0;
    bool confirm = false, run = true;
    while (aptMainLoop() && run) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        const int rows = 5;

        int move = 0;
        if (kd & KEY_DOWN) { move = 1; held = 0; }
        else if (kd & KEY_UP) { move = -1; held = 0; }
        else if (kh & (KEY_DOWN | KEY_UP)) {
            held++;
            if (held > 15 && held % 3 == 0) move = (kh & KEY_DOWN) ? 1 : -1;
        } else held = 0;
        if (kd & KEY_R || kd & KEY_RIGHT) move = rows;
        if (kd & KEY_L || kd & KEY_LEFT) move = -rows;

        if (confirm) {
            if (kd & KEY_SELECT) g_delSave = !g_delSave;
            if (kd & KEY_B) confirm = false;
            if (kd & KEY_A) { doDelete(); confirm = false; sel = 0; top = 0; }
        } else {
            if (kd & KEY_START) run = false;
            if (g_viewN > 0 && move) {
                sel += move;
                if (sel < 0) sel = 0;
                if (sel > g_viewN - 1) sel = g_viewN - 1;
            }
            if (kd & KEY_A && g_viewN > 0) g_titles[g_view[sel]].mark = !g_titles[g_view[sel]].mark;
            if (kd & KEY_B) for (int i = 0; i < g_count; i++) g_titles[i].mark = false;
            if (kd & KEY_Y) { g_showAll = !g_showAll; rebuildView(); sel = 0; top = 0; }
            if (kd & KEY_SELECT) g_delSave = !g_delSave;
            if ((kd & KEY_X) && markedCount() > 0) confirm = true;
        }
        if (sel >= g_viewN) sel = g_viewN ? g_viewN - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        if (top < 0) top = 0;

        drawMain(sel, top, confirm);
    }

    for (int i = 0; i < g_count; i++) if (g_titles[i].hasIcon) C3D_TexDelete(&g_titles[i].tex);
    free(g_titles); free(g_view); free(g_gsize); free(g_gid);
    if (g_fontKR) C2D_FontFree(g_fontKR);
    C2D_TextBufDelete(g_buf); C2D_TextBufDelete(g_tmp);
    fsExit(); amExit();
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
