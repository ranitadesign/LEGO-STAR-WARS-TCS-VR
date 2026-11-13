/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
// Launcher for the VR build, meant to be added to Steam as a non-Steam game.
// Lives next to LEGOStarWarsSaga_vr.exe; runs it with the game data folder as working directory and
// waits for it, so Steam tracks the session. Logs land in the "log" folder next to this launcher.
// The data folder is found on any PC (see find_data_directory), so the VR folder can be copied elsewhere.
// Build: zig cc -target x86-windows-gnu -O2 launcher.c -o launcher.exe -Wl,--subsystem,windows -ladvapi32
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define META_RUNTIME "C:\\Program Files\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_32.json"
#define STEAMVR_RUNTIME "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json"

static int file_exists(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A folder holding the game's data (GAME.DAT next to the episodes).
static int is_data_directory(const char *directory) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\GAME.DAT", directory);
    return directory[0] != 0 && file_exists(path);
}

// Steam libraries: Steam's own folder, then every "path" in steamapps\libraryfolders.vdf. Calls found() with
// each library until it returns 1.
static int for_each_steam_library(int (*found)(const char *library, char *out), char *out) {
    char steam[MAX_PATH], vdf[MAX_PATH], line[1024];
    DWORD size = sizeof(steam), type;
    HKEY key;
    FILE *file;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(key, "SteamPath", NULL, &type, (BYTE *)steam, &size) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(key);
        return 0;
    }
    RegCloseKey(key);
    for (char *c = steam; *c; ++c)
        if (*c == '/')
            *c = '\\';
    if (found(steam, out))
        return 1;
    snprintf(vdf, sizeof(vdf), "%s\\steamapps\\libraryfolders.vdf", steam);
    file = fopen(vdf, "r");
    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file)) {
        // "path"		"D:\\Program Files (x86)\\Steam"
        char library[MAX_PATH], *start = strstr(line, "\"path\""), *open, *close;
        int n = 0;
        if (start == NULL || (open = strchr(start + 6, '"')) == NULL || (close = strrchr(open + 1, '"')) == NULL ||
            close == open)
            continue;
        for (char *c = open + 1; c < close && n < MAX_PATH - 1; ++c) {
            if (c[0] == '\\' && c[1] == '\\')
                ++c;
            library[n++] = *c;
        }
        library[n] = 0;
        if (found(library, out)) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static int steam_game(const char *library, char *out) {
    snprintf(out, MAX_PATH, "%s\\steamapps\\common\\Lego Star Wars Saga", library);
    return is_data_directory(out);
}

static int steam_vr(const char *library, char *out) {
    snprintf(out, MAX_PATH, "%s\\steamapps\\common\\SteamVR\\steamxr_win32.json", library);
    return file_exists(out);
}

// Where the game's data is: saga_vr.ini data_dir=, a "datos" folder next to the launcher, the launcher's own
// folder, or the game's Steam install in any Steam library.
static int find_data_directory(const char *directory, const char *ini, char *out) {
    GetPrivateProfileStringA("vr", "data_dir", "", out, MAX_PATH, ini);
    if (is_data_directory(out))
        return 1;
    snprintf(out, MAX_PATH, "%s\\datos", directory);
    if (is_data_directory(out))
        return 1;
    snprintf(out, MAX_PATH, "%s", directory);
    if (is_data_directory(out))
        return 1;
    return for_each_steam_library(steam_game, out);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
    char directory[MAX_PATH], game[MAX_PATH], ini[MAX_PATH], data[MAX_PATH], log_path[MAX_PATH], message[1024];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE output, input;
    SYSTEMTIME now;
    char *slash;
    (void)instance, (void)previous, (void)command_line, (void)show;

    GetModuleFileNameA(NULL, directory, sizeof(directory));
    slash = strrchr(directory, '\\');
    if (slash)
        *slash = 0;
    snprintf(game, sizeof(game), "%s\\LEGOStarWarsSaga_vr.exe", directory);
    snprintf(ini, sizeof(ini), "%s\\saga_vr.ini", directory);

    if (!find_data_directory(directory, ini, data)) {
        snprintf(message, sizeof(message),
                 "The game data (GAME.DAT) was not found.\n\n"
                 "Install LEGO Star Wars: The Complete Saga on Steam, or write its folder in saga_vr.ini:\n\n"
                 "data_dir=C:\\path\\to\\Lego Star Wars Saga\n\nVR folder: %s",
                 directory);
        MessageBoxA(NULL, message, "LEGO Star Wars VR by RanitaVR", MB_ICONERROR);
        return 1;
    }

    // The game is 32-bit, so it needs a runtime's 32-bit manifest. Steam exports XR_RUNTIME_JSON pointing
    // at SteamVR's 64-bit one, which cannot load; choose explicitly from saga_vr.ini [vr] runtime=
    // meta (default, Quest Link / Air Link) | steamvr | system (keep the system's active runtime).
    {
        char runtime[MAX_PATH], steamvr[MAX_PATH];
        GetPrivateProfileStringA("vr", "runtime", "meta", runtime, sizeof(runtime), ini);
        if (_stricmp(runtime, "steamvr") == 0)
            SetEnvironmentVariableA("XR_RUNTIME_JSON",
                                    for_each_steam_library(steam_vr, steamvr) ? steamvr : STEAMVR_RUNTIME);
        else if (_stricmp(runtime, "system") == 0)
            SetEnvironmentVariableA("XR_RUNTIME_JSON", NULL);
        else if (_stricmp(runtime, "meta") == 0)
            SetEnvironmentVariableA("XR_RUNTIME_JSON", META_RUNTIME);
        else
            SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime); // a manifest path
    }

    // The game's console output (also its log) is captured into log\consola-<time>.txt.
    snprintf(log_path, sizeof(log_path), "%s\\log", directory);
    CreateDirectoryA(log_path, NULL);
    GetLocalTime(&now);
    snprintf(log_path, sizeof(log_path), "%s\\log\\consola-%04d%02d%02d-%02d%02d%02d.txt", directory, now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    output = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    // Started from Explorer or Steam this launcher has no standard handles; give the game real ones.
    input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
    if (output != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = input;
        snprintf(message, sizeof(message), "LEGO Star Wars VR by RanitaVR\r\nGame data: %s\r\n", data);
        WriteFile(output, message, (DWORD)strlen(message), &(DWORD){0}, NULL);
    }
    // The game shows its window with the start-up show command; ask for a normal, visible window.
    startup.dwFlags |= STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    if (!CreateProcessA(game, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, data, &startup, &process)) {
        snprintf(message, sizeof(message), "Could not start the game (error %lu).\n\nGame: %s\nData: %s",
                 GetLastError(), game, data);
        MessageBoxA(NULL, message, "LEGO Star Wars VR by RanitaVR", MB_ICONERROR);
        return 1;
    }
    // Bring the game window forward once it exists (Steam/Explorer may keep focus otherwise).
    for (int i = 0; i < 300; ++i) {
        HWND window = FindWindowA("TTalesWindow", NULL);
        DWORD owner = 0;
        if (window != NULL && GetWindowThreadProcessId(window, &owner) && owner == process.dwProcessId) {
            if (!IsWindowVisible(window))
                ShowWindow(window, SW_SHOWNORMAL);
            SetForegroundWindow(window);
            break;
        }
        if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0)
            break;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    return 0;
}
