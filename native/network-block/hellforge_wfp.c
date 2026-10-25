// HellForge's network block, placed straight into the Windows Filtering Platform (WFP) instead of as Windows
// Firewall rules. It does the same job as scripts/Install-NetworkBlock.ps1's firewall rules did: the copied game's
// programs reach the local backend's loopback port and nothing else. Putting the block in WFP directly means it holds
// even when Windows Firewall is switched off, and a high-weight sublayer makes our BLOCK win the platform's
// arbitration over a security suite's or VPN's own permit sublayer (the beta's Kaspersky/VPN problem).
//
//   hellforge_wfp install   <providerGuid> <displayName> <programsFile> <port> [reportFile]
//   hellforge_wfp uninstall <providerGuid> [reportFile]
//   hellforge_wfp status    <providerGuid> <programsFile> [reportFile]
//
// providerGuid identifies one HellForge installation's block (the caller derives it from the install path, as the
// firewall rule group was), so blocks for several installations never touch each other. displayName labels the WFP
// objects for anyone looking at them (e.g. in wf.msc). programsFile is one program path per line, UTF-8. Every
// command writes a one-line JSON report to reportFile (or stdout): {"status":"...", ...}. Exit code 0 on success.
//
// Adding or removing WFP objects needs administrator rights; run elevated. Objects are persistent, so the block
// survives a reboot until uninstall removes it, exactly as the firewall rules did.

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#define INITGUID
#include <guiddef.h>
#include <fwpmu.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// WFP layer and condition keys. The mingw headers declare the WFP functions and structs but not these well-known
// GUIDs (in the Windows SDK they come from uuid.lib), so we define them here with their documented values.
DEFINE_GUID(HF_LAYER_ALE_AUTH_CONNECT_V4,     0xc38d57d1,0x05a7,0x4c33,0x90,0x4f,0x7f,0xbc,0xee,0xe6,0x0e,0x82);
DEFINE_GUID(HF_LAYER_ALE_AUTH_CONNECT_V6,     0x4a72393b,0x319f,0x44bc,0x84,0xc3,0xba,0x54,0xdc,0xb3,0xb6,0xb4);
DEFINE_GUID(HF_LAYER_ALE_AUTH_RECV_ACCEPT_V4, 0xe1cd9fe7,0xf4b5,0x4273,0x96,0xc0,0x59,0x2e,0x48,0x7b,0x86,0x50);
DEFINE_GUID(HF_LAYER_ALE_AUTH_RECV_ACCEPT_V6, 0xa3b42c97,0x9f04,0x4672,0xb8,0x7e,0xce,0xe9,0xc4,0x83,0x25,0x7f);
DEFINE_GUID(HF_CONDITION_ALE_APP_ID,          0xd78e1e87,0x8644,0x4ea5,0x94,0x37,0xd8,0x09,0xec,0xef,0xc9,0x71);
DEFINE_GUID(HF_CONDITION_IP_REMOTE_ADDRESS,   0xb235ae9a,0x1d64,0x49b8,0xa4,0x4c,0x5f,0xf3,0xd9,0x09,0x50,0x45);
DEFINE_GUID(HF_CONDITION_IP_REMOTE_PORT,      0xc35a604d,0xd22b,0x4e1a,0x91,0xb4,0x68,0xf6,0x74,0xee,0x67,0x4b);

// One sublayer, shared by every HellForge installation's block, at the top weight so its BLOCK is authoritative.
// It is owned by no provider (providerKey NULL), so uninstalling one installation never needs to remove it.
DEFINE_GUID(HF_SUBLAYER, 0x4e6f2b1a,0x9d3c,0x4f7a,0xb2,0x10,0x8c,0x55,0xd1,0x3e,0x77,0x90);

#ifndef FWPM_PROVIDER_FLAG_PERSISTENT
#define FWPM_PROVIDER_FLAG_PERSISTENT 0x00000001
#endif
#ifndef FWPM_SUBLAYER_FLAG_PERSISTENT
#define FWPM_SUBLAYER_FLAG_PERSISTENT 0x00000001
#endif
#ifndef FWPM_FILTER_FLAG_PERSISTENT
#define FWPM_FILTER_FLAG_PERSISTENT 0x00000001
#endif
#ifndef RPC_C_AUTHN_WINNT
#define RPC_C_AUTHN_WINNT 10
#endif

static const UINT8 WEIGHT_PERMIT = 0x0f; // loopback is permitted over the block beneath it
static const UINT8 WEIGHT_BLOCK  = 0x08;
static UINT16 gPort = 0; // install: the one loopback port the outbound permit allows (the backend's)

// The four ALE layers the block covers: outbound (connect) and inbound (recv/accept), v4 and v6. ALE_AUTH_CONNECT is
// classified for TCP connects and the first outbound UDP packet both, so UDP is covered without a separate layer.
static const GUID *const LAYERS[4] = {
    &HF_LAYER_ALE_AUTH_CONNECT_V4, &HF_LAYER_ALE_AUTH_CONNECT_V6,
    &HF_LAYER_ALE_AUTH_RECV_ACCEPT_V4, &HF_LAYER_ALE_AUTH_RECV_ACCEPT_V6,
};
static int LayerIsV6(const GUID *layer){ return layer == &HF_LAYER_ALE_AUTH_CONNECT_V6 || layer == &HF_LAYER_ALE_AUTH_RECV_ACCEPT_V6; }
static int LayerIsConnect(const GUID *layer){ return layer == &HF_LAYER_ALE_AUTH_CONNECT_V4 || layer == &HF_LAYER_ALE_AUTH_CONNECT_V6; }

#define FILTERS_PER_PROGRAM 8 // (permit loopback + block) over the four layers

static wchar_t *gDisplayName = L"HellForge network block";

// A JSON string escaper for the report (paths and messages). Writes into out (bounded by cap), returns out.
static void JsonEscape(FILE *f, const char *s){
    fputc('"', f);
    for(; *s; ++s){
        unsigned char c = (unsigned char)*s;
        if(c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if(c == '\n') fputs("\\n", f);
        else if(c == '\r') fputs("\\r", f);
        else if(c == '\t') fputs("\\t", f);
        else if(c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

// Write the one-line JSON report to reportPath (or stdout) and return the process exit code (0 = success).
// failure is NULL on success.
static int Report(const wchar_t *reportPath, const char *status, const char *action,
                  int programs, int filters, const char *failure){
    FILE *f = reportPath ? _wfopen(reportPath, L"wb") : NULL;
    if(!f) f = stdout; // no report path, or it could not be opened: say it on stdout
    fprintf(f, "{\"action\":\"%s\",\"status\":\"%s\"", action, status);
    if(programs >= 0) fprintf(f, ",\"programs\":%d", programs);
    if(filters >= 0)  fprintf(f, ",\"filters\":%d", filters);
    if(gPort > 0)     fprintf(f, ",\"port\":%u", (unsigned)gPort);
    if(failure){ fputs(",\"failure\":", f); JsonEscape(f, failure); }
    fputs("}\n", f);
    if(f != stdout) fclose(f);
    return (strcmp(status, "failed") == 0) ? 1 : 0;
}

// Read programsFile into a NULL-terminated array of wide paths (caller frees array and each entry). Blank lines
// and a leading UTF-8 BOM are skipped. Returns NULL on read failure (count set to -1).
static wchar_t **ReadPrograms(const wchar_t *path, int *count){
    *count = -1;
    FILE *f = _wfopen(path, L"rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc((size_t)len + 1);
    size_t got = fread(data, 1, (size_t)len, f); data[got] = 0; fclose(f);
    size_t start = 0;
    if(got >= 3 && (unsigned char)data[0]==0xEF && (unsigned char)data[1]==0xBB && (unsigned char)data[2]==0xBF) start = 3;
    wchar_t **list = (wchar_t**)calloc(got + 2, sizeof(wchar_t*)); // generous upper bound
    int n = 0;
    size_t i = start;
    while(i < got){
        size_t j = i;
        while(j < got && data[j] != '\n' && data[j] != '\r') ++j;
        size_t lineLen = j - i;
        while(lineLen > 0 && (data[i + lineLen - 1] == ' ' || data[i + lineLen - 1] == '\t')) --lineLen;
        if(lineLen > 0){
            int wn = MultiByteToWideChar(CP_UTF8, 0, data + i, (int)lineLen, NULL, 0);
            wchar_t *w = (wchar_t*)malloc(((size_t)wn + 1) * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, data + i, (int)lineLen, w, wn); w[wn] = 0;
            list[n++] = w;
        }
        i = j;
        while(i < got && (data[i] == '\n' || data[i] == '\r')) ++i;
    }
    list[n] = NULL;
    free(data);
    *count = n;
    return list;
}

static void FreePrograms(wchar_t **list){ if(!list) return; for(int i = 0; list[i]; ++i) free(list[i]); free(list); }

// Add the permit-loopback + block pair for one app id on one layer, inside our transaction.
static DWORD AddPair(HANDLE engine, const GUID *provider, const GUID *layer, FWP_BYTE_BLOB *appId){
    int v6 = LayerIsV6(layer);
    // Loopback as an address+mask: 127.0.0.0/8 for v4, ::1/128 for v6.
    FWP_V4_ADDR_AND_MASK v4 = { 0x7f000000u, 0xff000000u };
    FWP_V6_ADDR_AND_MASK v6m; memset(&v6m, 0, sizeof v6m); v6m.addr[15] = 1; v6m.prefixLength = 128;

    for(int permit = 1; permit >= 0; --permit){ // permit filter first, then the block beneath it
        FWPM_FILTER_CONDITION0 cond[3];
        memset(cond, 0, sizeof cond);
        UINT32 nCond = 0;

        cond[nCond].fieldKey = HF_CONDITION_ALE_APP_ID;
        cond[nCond].matchType = FWP_MATCH_EQUAL;
        cond[nCond].conditionValue.type = FWP_BYTE_BLOB_TYPE;
        cond[nCond].conditionValue.byteBlob = appId;
        ++nCond;

        if(permit){
            cond[nCond].fieldKey = HF_CONDITION_IP_REMOTE_ADDRESS;
            cond[nCond].matchType = FWP_MATCH_EQUAL;
            if(v6){ cond[nCond].conditionValue.type = FWP_V6_ADDR_MASK; cond[nCond].conditionValue.v6AddrMask = &v6m; }
            else  { cond[nCond].conditionValue.type = FWP_V4_ADDR_MASK; cond[nCond].conditionValue.v4AddrMask = &v4; }
            ++nCond;
            if(gPort > 0 && LayerIsConnect(layer)){
                cond[nCond].fieldKey = HF_CONDITION_IP_REMOTE_PORT;
                cond[nCond].matchType = FWP_MATCH_EQUAL;
                cond[nCond].conditionValue.type = FWP_UINT16;
                cond[nCond].conditionValue.uint16 = gPort;
                ++nCond;
            }
        }

        FWPM_FILTER0 filter;
        memset(&filter, 0, sizeof filter);
        filter.displayData.name = gDisplayName;
        filter.flags = FWPM_FILTER_FLAG_PERSISTENT;
        filter.providerKey = (GUID*)provider;
        filter.layerKey = *layer;
        filter.subLayerKey = HF_SUBLAYER;
        filter.weight.type = FWP_UINT8;
        filter.weight.uint8 = permit ? WEIGHT_PERMIT : WEIGHT_BLOCK;
        filter.numFilterConditions = nCond;
        filter.filterCondition = cond;
        filter.action.type = permit ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
        // filterKey left zero: WFP assigns one. The provider key is how we find and remove these later.

        DWORD r = FwpmFilterAdd0(engine, &filter, NULL, NULL);
        if(r != ERROR_SUCCESS) return r;
    }
    return ERROR_SUCCESS;
}

static DWORD EnsureProvider(HANDLE engine, const GUID *provider){
    FWPM_PROVIDER0 p; memset(&p, 0, sizeof p);
    p.providerKey = *provider;
    p.displayData.name = gDisplayName;
    p.flags = FWPM_PROVIDER_FLAG_PERSISTENT;
    DWORD r = FwpmProviderAdd0(engine, &p, NULL);
    return (r == FWP_E_ALREADY_EXISTS) ? ERROR_SUCCESS : r;
}

static DWORD EnsureSubLayer(HANDLE engine){
    FWPM_SUBLAYER0 s; memset(&s, 0, sizeof s);
    s.subLayerKey = HF_SUBLAYER;
    s.displayData.name = L"HellForge network block";
    s.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
    s.weight = 0xffff; // above third-party sublayers, so our BLOCK is authoritative
    DWORD r = FwpmSubLayerAdd0(engine, &s, NULL);
    return (r == FWP_E_ALREADY_EXISTS) ? ERROR_SUCCESS : r;
}

// Count or delete every filter tagged with our provider. If deleteThem, removes them; otherwise just counts.
// Returns the count (>=0) or -1 on enumeration failure.
static int ForEachProviderFilter(HANDLE engine, const GUID *provider, int deleteThem){
    HANDLE enumHandle = NULL;
    if(FwpmFilterCreateEnumHandle0(engine, NULL, &enumHandle) != ERROR_SUCCESS) return -1;
    int count = 0; int ok = 1;
    for(;;){
        FWPM_FILTER0 **items = NULL; UINT32 n = 0;
        DWORD r = FwpmFilterEnum0(engine, enumHandle, 64, &items, &n);
        if(r != ERROR_SUCCESS){ ok = 0; break; }
        if(n == 0){ if(items) FwpmFreeMemory0((void**)&items); break; }
        for(UINT32 i = 0; i < n; ++i){
            FWPM_FILTER0 *flt = items[i];
            if(flt->providerKey && IsEqualGUID(flt->providerKey, provider)){
                ++count;
                if(deleteThem && FwpmFilterDeleteById0(engine, flt->filterId) != ERROR_SUCCESS) ok = 0;
            }
        }
        FwpmFreeMemory0((void**)&items);
    }
    FwpmFilterDestroyEnumHandle0(engine, enumHandle);
    return ok ? count : -1;
}

static int ParseGuid(const wchar_t *s, GUID *out){
    // Accept both {..} and bare forms.
    wchar_t braced[64];
    if(s[0] == L'{') return CLSIDFromString((LPOLESTR)s, out) == NOERROR;
    _snwprintf(braced, 64, L"{%s}", s); braced[63] = 0;
    return CLSIDFromString((LPOLESTR)braced, out) == NOERROR;
}

int wmain(int argc, wchar_t **argv){
    if(argc < 3){
        fprintf(stderr, "usage: hellforge_wfp install|uninstall|status <providerGuid> [...]\n");
        return 2;
    }
    const wchar_t *cmd = argv[1];
    GUID provider;
    if(!ParseGuid(argv[2], &provider)){ return Report(NULL, "failed", "unknown", -1, -1, "Bad provider GUID."); }

    int isInstall  = (wcscmp(cmd, L"install") == 0);
    int isUninstall = (wcscmp(cmd, L"uninstall") == 0);
    int isStatus   = (wcscmp(cmd, L"status") == 0);
    if(!isInstall && !isUninstall && !isStatus) return Report(NULL, "failed", "unknown", -1, -1, "Unknown command.");
    const char *action = isInstall ? "install" : isUninstall ? "uninstall" : "status";

    const wchar_t *programsFile = NULL, *reportFile = NULL;
    if(isInstall){
        if(argc < 6) return Report(NULL, "failed", action, -1, -1, "install needs <providerGuid> <displayName> <programsFile> <port> [reportFile].");
        gDisplayName = argv[3];
        programsFile = argv[4];
        reportFile = argc >= 7 ? argv[6] : NULL;
        // Digits only, 1-65535: a port that does not parse must never turn into 0, which would permit every port.
        wchar_t *end = NULL;
        unsigned long portVal = (argv[5][0] >= L'0' && argv[5][0] <= L'9') ? wcstoul(argv[5], &end, 10) : 0;
        if(!end || *end != L'\0' || portVal < 1 || portVal > 65535)
            return Report(reportFile, "failed", action, -1, -1, "The port must be a number from 1 to 65535.");
        gPort = (UINT16)portVal;
    } else if(isUninstall){
        reportFile = argc >= 4 ? argv[3] : NULL;
    } else { // status
        if(argc < 4) return Report(NULL, "failed", action, -1, -1, "status needs <providerGuid> <programsFile> [reportFile].");
        programsFile = argv[3];
        reportFile = argc >= 5 ? argv[4] : NULL;
    }

    wchar_t **programs = NULL; int nPrograms = -1;
    if(programsFile){
        programs = ReadPrograms(programsFile, &nPrograms);
        if(!programs || nPrograms < 0) return Report(reportFile, "failed", action, -1, -1, "Could not read the programs list.");
        if(nPrograms == 0 && isInstall) { FreePrograms(programs); return Report(reportFile, "failed", action, 0, -1, "The programs list is empty."); }
    }

    HANDLE engine = NULL;
    DWORD r = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, &engine);
    if(r != ERROR_SUCCESS){
        char msg[128]; _snprintf(msg, sizeof msg, "Could not open the filtering engine (0x%08lx). Run as administrator.", (unsigned long)r);
        FreePrograms(programs);
        return Report(reportFile, "failed", action, nPrograms, -1, msg);
    }

    if(isStatus){
        int count = ForEachProviderFilter(engine, &provider, 0);
        FwpmEngineClose0(engine);
        int expected = nPrograms * FILTERS_PER_PROGRAM;
        const char *status = (count < 0) ? "failed" : (count >= expected && expected > 0 ? "installed" : "not_installed");
        const char *failure = (count < 0) ? "Could not read the filters." : NULL;
        int rc = Report(reportFile, status, action, nPrograms, count < 0 ? -1 : count, failure);
        FreePrograms(programs);
        return rc;
    }

    if(isUninstall){
        r = FwpmTransactionBegin0(engine, 0);
        int removed = -1; const char *failure = NULL;
        if(r == ERROR_SUCCESS){
            removed = ForEachProviderFilter(engine, &provider, 1);
            if(removed < 0){ failure = "Could not remove every filter."; }
            else{
                DWORD d = FwpmProviderDeleteByKey0(engine, &provider);
                if(d != ERROR_SUCCESS && d != FWP_E_PROVIDER_NOT_FOUND) failure = "Could not remove the provider.";
            }
        } else failure = "Could not begin the transaction.";
        if(failure){ FwpmTransactionAbort0(engine); }
        else if(FwpmTransactionCommit0(engine) != ERROR_SUCCESS) failure = "Could not commit the changes.";
        FwpmEngineClose0(engine);
        FreePrograms(programs);
        return Report(reportFile, failure ? "failed" : "uninstalled", action, -1, removed, failure);
    }

    // install
    r = FwpmTransactionBegin0(engine, 0);
    const char *failure = NULL; int added = 0;
    if(r != ERROR_SUCCESS) failure = "Could not begin the transaction.";
    if(!failure && EnsureProvider(engine, &provider) != ERROR_SUCCESS) failure = "Could not add the provider.";
    if(!failure && EnsureSubLayer(engine) != ERROR_SUCCESS) failure = "Could not add the sublayer.";
    // Remove any filters left from an earlier install of this provider, so a re-run is clean, not doubled.
    if(!failure && ForEachProviderFilter(engine, &provider, 1) < 0) failure = "Could not clear earlier filters.";
    for(int p = 0; !failure && programs[p]; ++p){
        FWP_BYTE_BLOB *appId = NULL;
        DWORD g = FwpmGetAppIdFromFileName0(programs[p], &appId);
        if(g != ERROR_SUCCESS || !appId){ failure = "Could not read a program's application id."; break; }
        for(int L = 0; L < 4; ++L){
            DWORD a = AddPair(engine, &provider, LAYERS[L], appId);
            if(a != ERROR_SUCCESS){ static char m[96]; _snprintf(m, sizeof m, "Could not add a filter (layer %d, 0x%08lx).", L, (unsigned long)a); failure = m; break; }
            added += 2;
        }
        FwpmFreeMemory0((void**)&appId);
    }
    if(failure){ FwpmTransactionAbort0(engine); }
    else if(FwpmTransactionCommit0(engine) != ERROR_SUCCESS) failure = "Could not commit the changes.";
    FwpmEngineClose0(engine);
    int rc = Report(reportFile, failure ? "failed" : "installed", action, nPrograms, failure ? -1 : added, failure);
    FreePrograms(programs);
    return rc;
}
