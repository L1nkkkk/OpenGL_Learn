#include "Global.h"

int SCREEN_WIDTH = 1440;
int SCREEN_HEIGHT = 900;

int USED_TEXTURE_NUM = 0;

int SHADOW_WIDTH = 1024;
int SHADOW_HEIGHT = 1024;
bool SHADOW_MAP_SHOW = false;
int SHADOW_PCF_SAMPLE_NUM = 16;
int SHADOW_PCF_RING_NUM = 10;
int SHADOW_TYPE = ShadowProperty::Default;

bool GAMMA_CORRECTION = true;
float GAMMA_VALUE = 2.2f;

bool USE_HDR = false;
float HDR_EXPOSURE = 1.0;