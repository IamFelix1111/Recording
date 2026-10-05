// String table IDs - all user-visible GUI text lives in gui.rc for translation.
#pragma once

// Window / titles
#define IDS_APP_TITLE        100
#define IDS_TITLE_REC_FMT    101  // "(%ls) %ls  %ls %.1f fps"
#define IDS_TITLE_STATE_FMT  102  // "(%ls) %ls"

// States / status
#define IDS_STATE_READY      110
#define IDS_STATE_PREPARING  111
#define IDS_STATE_RECORDING  112
#define IDS_STATE_PAUSED     113
#define IDS_STATE_SAVING     114
#define IDS_STATUS_SAVED     116
#define IDS_STATUS_ERROR     117

// Buttons
#define IDS_BTN_START        120
#define IDS_BTN_STOP         121
#define IDS_BTN_PAUSE        122
#define IDS_BTN_RESUME       123
#define IDS_BROWSE           124

// Settings labels
#define IDS_LBL_MONITOR      130
#define IDS_LBL_FPS          131
#define IDS_LBL_BITRATE      132
#define IDS_LBL_PATH         133
#define IDS_CHK_CURSOR       134
#define IDS_CHK_GDI          135
#define IDS_MONITOR_ALL      136
#define IDS_MONITOR_FMT      137  // "%d: %ldx%ld%ls"
#define IDS_MONITOR_PRIMARY  138
#define IDS_BITRATE_AUTO     139

// Stats (fixed layout, one control each)
#define IDS_STATS_TIME_FMT   140   // "时长 %ls"
#define IDS_STATS_FRAMES_FMT 141   // "帧数 %llu"
#define IDS_STATS_FPS_FMT    142   // "%.1f fps"
#define IDS_STATS_SIZE_FMT   143   // "%.1f MB"
#define IDS_STATS_AVG_FMT    144   // "平均 %.1f fps"

// Dialogs
#define IDS_ERR_TITLE        150
#define IDS_CONFIRM_QUIT     151
