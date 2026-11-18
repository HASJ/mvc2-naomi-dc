/*
 * nl_am.c  The NAOMI amusement library (nla*): fixed settings, the JVS switch report from the host's
 * buttons, credits, backup memory and EEPROM as files, the clock
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "naomi.h"
#include "naomi_host.h"
#include "naomi_input.h"

static NLA_SYSTEM_FLAG system_flag = {
    NLA_COUNTRY_USA, NLA_ADVSND_ON, NLA_MONITOR_HORIZONTAL, NLA_DISPLAY_31K, { 'B', 'B', 'G', '0' },
};

static NLA_CREDIT_CONF credit_conf_data = {
    9,                      /* MaxCredit */
    NLA_CHUTE_2,            /* CabinetType: two players */
    NLA_CHUTE_COMMON,
    NLA_COINOPE,
    1,
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1 },
    0,
    1,
};

void nlaGetBootFlag(void) {
}

void nlaInitSystem(void) {
}

NLA_SYSTEM_FLAG* nlaGetSystemFlag(void) {
    return &system_flag;
}

NLA_CREDIT_CONF* nlaGetCreditConf(void) {
    return &credit_conf_data;
}

int nlaROMTypeSet(int type, int a) {
    (void)type;
    (void)a;
    return 0;
}

void nlaInTestMode(void) {
}

void nlaJumpTestMode(void) {
    host_log("the game asked for the board's test menu");
}

void nlaPutLED(unsigned char led) {
    (void)led;
}

int nlaJAMMANodes;
NLA_JAMMA_INFO nlaJAMMAInfo[31];
char nlaJAMMAInput[31][512];
int nlaJAMMAAcknowledgeIndex[31][256];
unsigned char nlaInput6149[16];         /* [5]: test and service, active low */

void nlaJAMMAInit(void) {
    nlaJAMMANodes = 1;
    nlaJAMMAInfo[1].node = 1;
    strcpy(nlaJAMMAInfo[1].IDCode, "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551;Ver1.00");
    nlaJAMMAInfo[1].Switch[0] = 2;      /* players */
    nlaJAMMAInfo[1].Switch[1] = 13;     /* switches each */
    nlaJAMMAInfo[1].Coin = 2;
    nlaJAMMAAcknowledgeIndex[1][NLA_JAMMA_COMMAND_SWITCH_READ] = 0;
    nlaJAMMAAcknowledgeIndex[1][NLA_JAMMA_COMMAND_ANALOG_READ] = 6;
}

void nlaJAMMACreditInit(void) {
}

void nlaJAMMASetPrimaryNode(int node) {
    (void)node;
}

static NLA_CREDIT_INFO credits;
static NLA_BACKUP_CREDIT backup_credit;     /* the boot ROM's coin book-keeping */

static void credit_status(int pl, int sequence) {
    NLA_CREDIT_DATA* p = &credits.Player[pl];
    uint32_t need = credit_conf_data.Sequence[sequence & 7];
    p->Status = p->Credit >= need ? NLA_CREDIT_ENOUGH : p->Credit ? NLA_CREDIT_MORE : NLA_CREDIT_ZERO;
}

static void add_credit(void) {
    NLA_CREDIT_DATA* p = &credits.Player[0];
    if (credit_conf_data.CoinFlag == NLA_FREEPLAY || p->Credit >= credit_conf_data.MaxCredit) {
        return;
    }
    p->Credit++;
    credits.Player[1].Credit = p->Credit;   /* a common chute: both sides share the credits */
    backup_credit.CoinChute[0]++;
    backup_credit.TotalCoin++;
    backup_credit.CoinCredit++;
    backup_credit.TotalCredit++;
}

void nlaGetCreditData(NLA_CREDIT_INFO* info) {
    int i;
    credits.CoinFlag = credit_conf_data.CoinFlag;
    credits.ChuteType = credit_conf_data.ChuteType;
    credits.Chute = 2;
    for (i = 0; i < NLA_CREDIT_SEQUENCE; i++) {
        credits.Sequence[i] = credit_conf_data.Sequence[i];
    }
    for (i = 0; i < 2; i++) {
        credits.Player[i].BaseRate = 1;
        credit_status(i, 0);
    }
    *info = credits;
}

int nlaCheckCreditEnough(int pl, int sequence) {
    if (credit_conf_data.CoinFlag == NLA_FREEPLAY) {
        return 1;
    }
    return credits.Player[pl & 1].Credit >= credit_conf_data.Sequence[sequence & 7];
}

void nlaChangeCreditSequence(int pl, int sequence) {
    uint32_t need = credit_conf_data.Sequence[sequence & 7];
    if (credit_conf_data.CoinFlag == NLA_FREEPLAY) {
        return;
    }
    if (credits.Player[0].Credit >= need) {
        credits.Player[0].Credit -= need;
        credits.Player[1].Credit = credits.Player[0].Credit;
    }
    (void)pl;
}

static char* user_backup;
static int user_backup_size;
static char* user_eeprom;
static int user_eeprom_size;

static int load_file(const char* name, char* p, int size) {
    FILE* f = fopen(name, "rb");
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = fread(p, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

/* through a temporary file, never half written */
static void save_file(const char* name, const char* p, int size) {
    char tmp[256];
    FILE* f;
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    if (fwrite(p, 1, (size_t)size, f) == (size_t)size && fclose(f) == 0) {
        remove(name);
        rename(tmp, name);
    } else {
        fclose(f);
    }
}

/* a test run keeps them in the current folder */
static const char* save_name(const char* name) {
    return getenv("MVC2_FRAMES") != NULL ? name : host_save_path(name);
}

/* 0 when the saved data was there; otherwise the game sets its defaults */
int nlaInitUserBackup(int size, char* sdram) {
    user_backup = sdram;
    user_backup_size = size;
    return load_file(save_name("backup.bin"), sdram, size) ? 0 : -1;
}

int nlaInitUserEEPROM(int size, char* sdram) {
    user_eeprom = sdram;
    user_eeprom_size = size;
    return load_file(save_name("eeprom.bin"), sdram, size) ? 0 : -1;
}

void nlaUpdateUserBackup(void) {
    if (user_backup != NULL) {
        save_file(save_name("backup.bin"), user_backup, user_backup_size);
    }
}

void nlaUpdateUserEEPROM(void) {
    if (user_eeprom != NULL) {
        save_file(save_name("eeprom.bin"), user_eeprom, user_eeprom_size);
    }
}

void nlaUpdateSystemBackup(void) {
}

void nlaGetBackupData(NLA_BACKUP_BANK* bank) {
    memset(bank, 0, sizeof *bank);
    bank->credit = backup_credit;
    bank->usersize = (Uint32)user_backup_size;
    bank->usersram = (Uint32*)user_backup;
    bank->userdata = (Uint32*)user_backup;
}

/* a test run's MVC2_CLOCK (seconds from 1970, UTC) fixes it; read once */
static int netplay_canonical;
static int clock_override_on;
static time_t clock_override_t = 946684800;

void host_set_netplay_canonical(int on) {
    netplay_canonical = on ? 1 : 0;
}

int host_is_netplay_canonical(void) {
    return netplay_canonical;
}

void host_clock_set_override(int on, long long unix_sec) {
    clock_override_on = on ? 1 : 0;
    if (clock_override_on) {
        clock_override_t = (time_t)unix_sec;
    }
}

void host_clock(struct tm* out) {
    static int looked, fixed;
    static time_t fixed_t;
    time_t t;
    struct tm* tm;
    if (clock_override_on) {
        t = clock_override_t;
        tm = gmtime(&t);
        *out = *tm;
        return;
    }
    if (!looked) {
        const char* e = getenv("MVC2_FRAMES") != NULL ? getenv("MVC2_CLOCK") : NULL;
        fixed = e != NULL;
        fixed_t = fixed ? (time_t)strtoll(e, NULL, 10) : 0;
        looked = 1;
    }
    t = fixed ? fixed_t : time(NULL);
    tm = fixed ? gmtime(&t) : localtime(&t);
    *out = *tm;
}

int host_unlock;

int host_is_unlocked(void) {
    return host_unlock || netplay_canonical;
}

void nlaGetRTC(NLA_RTC_DATE* date) {
    struct tm now, *tm = &now;
    host_clock(&now);
    memset(date, 0, sizeof *date);
    date->year = (Uint16)(tm->tm_year + 1900);
    date->month = (Uint8)(tm->tm_mon + 1);
    date->day = (Uint8)tm->tm_mday;
    date->hour = (Uint8)tm->tm_hour;
    date->minute = (Uint8)tm->tm_min;
    date->second = (Uint8)tm->tm_sec;
    date->dayofweek = (Uint8)tm->tm_wday;
}

/* system (bit 7 test), then two bytes a player: start, service, up, down, left, right, push 1 to 8 */
void nla_frame_input(void) {
    const HOST_INPUT* in = host_input();
    char* r = nlaJAMMAInput[1];      /* node 1's acknowledge, read through nlaJAMMAInput[0] + 512 */
    int pl;
    r[0] = NLA_JAMMA_REPORT_OK;
    r[1] = (char)(in->test ? 0x80 : 0);
    for (pl = 0; pl < 2; pl++) {
        uint32_t b = in->player[pl];
        r[2 + pl * 2] = (char)(b >> 8);
        r[3 + pl * 2] = (char)b;
    }
    r[6] = 0;       /* no analog report */
    nlaInput6149[5] = 0xFF;
    if (in->coin) {
        add_credit();
    }
}

typedef struct {
    NLA_SYSTEM_FLAG system_flag;
    NLA_CREDIT_CONF credit_conf_data;
    int jamma_nodes;
    NLA_JAMMA_INFO jamma_info[31];
    char jamma_input[31][512];
    int jamma_ack[31][256];
    unsigned char input_6149[16];
    NLA_CREDIT_INFO credits;
    NLA_BACKUP_CREDIT backup_credit;
} NL_AM_STATE;

size_t nl_am_state_size(void) {
    return sizeof(NL_AM_STATE);
}

void nl_am_state_save(void* out) {
    NL_AM_STATE s;
    s.system_flag = system_flag;
    s.credit_conf_data = credit_conf_data;
    s.jamma_nodes = nlaJAMMANodes;
    memcpy(s.jamma_info, nlaJAMMAInfo, sizeof nlaJAMMAInfo);
    memcpy(s.jamma_input, nlaJAMMAInput, sizeof nlaJAMMAInput);
    memcpy(s.jamma_ack, nlaJAMMAAcknowledgeIndex, sizeof nlaJAMMAAcknowledgeIndex);
    memcpy(s.input_6149, nlaInput6149, sizeof nlaInput6149);
    s.credits = credits;
    s.backup_credit = backup_credit;
    memcpy(out, &s, sizeof s);
}

void nl_am_state_load(const void* in) {
    NL_AM_STATE s;
    memcpy(&s, in, sizeof s);
    system_flag = s.system_flag;
    credit_conf_data = s.credit_conf_data;
    nlaJAMMANodes = s.jamma_nodes;
    memcpy(nlaJAMMAInfo, s.jamma_info, sizeof nlaJAMMAInfo);
    memcpy(nlaJAMMAInput, s.jamma_input, sizeof nlaJAMMAInput);
    memcpy(nlaJAMMAAcknowledgeIndex, s.jamma_ack, sizeof nlaJAMMAAcknowledgeIndex);
    memcpy(nlaInput6149, s.input_6149, sizeof nlaInput6149);
    credits = s.credits;
    backup_credit = s.backup_credit;
}
