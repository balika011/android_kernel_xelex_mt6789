#ifndef PANEL_INNOLUX_PD035SC_01A
#define PANEL_INNOLUX_PD035SC_01A

#define REGFLAG_CMD             0xFFFA
#define REGFLAG_DELAY           0xFFFC
#define REGFLAG_UDELAY          0xFFFB
#define REGFLAG_END_OF_TABLE    0xFFFD
#define REGFLAG_RESET_LOW       0xFFFE
#define REGFLAG_RESET_HIGH      0xFFFF

#define FRAME_WIDTH                 720
#define FRAME_HEIGHT                720

#define PHYSICAL_WIDTH              62000
#define PHYSICAL_HEIGHT             62000

#define DATA_RATE                   652
#define PLL_CLK                     326

#define HSA                         50
#define HBP                         40
#define HFP                         60
#define VSA                         5
#define VBP                         35
#define VFP                         68

#define FPS                         60

#endif //end of PANEL_INNOLUX_PD035SC_01A
