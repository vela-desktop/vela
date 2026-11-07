// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST ONLY: KDE's private fake-input v4 wire ABI. Never installed by setup.
#include <wayland-client.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static struct wl_proxy *input;
static const struct wl_message requests[]={
 {"authenticate","ss",NULL},{"pointer_motion","ff",NULL},{"button","uu",NULL},{"axis","uf",NULL},
 {"touch_down","2iff",NULL},{"touch_motion","2iff",NULL},{"touch_up","2i",NULL},
 {"touch_cancel","2",NULL},{"touch_frame","2",NULL},{"pointer_motion_absolute","3ff",NULL},{"keyboard_key","4uu",NULL}};
static const struct wl_interface interface={"org_kde_kwin_fake_input",4,11,requests,0,NULL};
static void global(void *data,struct wl_registry *registry,uint32_t name,const char *iface,uint32_t version)
{(void)data;if(!strcmp(iface,interface.name) && version>=4)input=wl_registry_bind(registry,name,&interface,4);}
static void removed(void *data,struct wl_registry *registry,uint32_t name){(void)data;(void)registry;(void)name;}
static const struct wl_registry_listener listener={global,removed};
static unsigned long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;}
static void key(unsigned code,unsigned state){wl_proxy_marshal_flags(input,10,NULL,4,0,code,state);}
static void pulse(unsigned code){key(code,1);key(code,0);}
int main(void)
{
 if(!getuid())return 77;
 struct wl_display *display=wl_display_connect(NULL);if(!display)return 77;
 wl_registry_add_listener(wl_display_get_registry(display),&listener,NULL);wl_display_roundtrip(display);
 if(!input)return 77;
 wl_proxy_marshal_flags(input,0,NULL,4,0,"Vela private development fixture","Private KWin automated tests");wl_display_roundtrip(display);
 puts("{\"event\":\"uinput-ready\",\"method\":\"linux-uinput-fixed-desktop-v1\",\"clock\":\"CLOCK_MONOTONIC\",\"sysfs\":\"/sys/devices/virtual/input/input999999999\"}");fflush(stdout);
 char command[80];unsigned index=0;
 while(fgets(command,sizeof command,stdin)) {
  if(!strcmp(command,"quit\n"))break;
  unsigned long long before=now();
  if(!strcmp(command,"alt-tab-begin\n")){key(KEY_LEFTALT,1);pulse(KEY_TAB);}
  else if(!strcmp(command,"alt-tab-end\n"))key(KEY_LEFTALT,0);
  else if(!strcmp(command,"window-menu-plasma\n")){key(KEY_LEFTALT,1);pulse(KEY_F3);key(KEY_LEFTALT,0);}
  else if(!strcmp(command,"launcher\n"))pulse(KEY_LEFTMETA);
  else if(!strcmp(command,"escape\n"))pulse(KEY_ESC);
  else if(!strcmp(command,"pulse\n"))pulse(KEY_F24);
  else if(!strcmp(command,"enter\n"))pulse(KEY_ENTER);
  else if(!strcmp(command,"search\n")){unsigned codes[]={KEY_V,KEY_E,KEY_L,KEY_A,KEY_B,KEY_E,KEY_N,KEY_C,KEY_H,KEY_S,KEY_C,KEY_E,KEY_N,KEY_E};for(unsigned i=0;i<sizeof codes/sizeof *codes;++i)pulse(codes[i]);}
  else if(!strcmp(command,"close\n")){key(KEY_LEFTALT,1);pulse(KEY_F4);key(KEY_LEFTALT,0);}
  else if(!strcmp(command,"down\n") || !strcmp(command,"up\n"))wl_proxy_marshal_flags(input,2,NULL,4,0,BTN_LEFT,!strcmp(command,"down\n")?1u:0u);
  else if(!strcmp(command,"desktop\n")){key(KEY_LEFTMETA,1);pulse(KEY_D);key(KEY_LEFTMETA,0);}
  else if(!strcmp(command,"workspace-first-plasma\n") || !strcmp(command,"workspace-second-plasma\n")){key(KEY_LEFTCTRL,1);pulse(!strcmp(command,"workspace-first-plasma\n")?KEY_F1:KEY_F2);key(KEY_LEFTCTRL,0);}
  else if(!strcmp(command,"snap-left\n") || !strcmp(command,"snap-right\n") || !strcmp(command,"maximize-toggle\n")){key(KEY_LEFTMETA,1);pulse(!strcmp(command,"snap-left\n")?KEY_LEFT:!strcmp(command,"snap-right\n")?KEY_RIGHT:KEY_PAGEUP);key(KEY_LEFTMETA,0);}
  else {unsigned x,y;char extra;if(sscanf(command,"point %u %u %c",&x,&y,&extra)!=2 || x>65535 || y>65535)return 1;
   wl_proxy_marshal_flags(input,9,NULL,4,0,wl_fixed_from_double(x*1919./65535),wl_fixed_from_double(y*1079./65535));}
  if(wl_display_roundtrip(display)<0)return 1;
  printf("{\"event\":\"injected\",\"action_id\":%u,\"before_monotonic_ns\":%llu,\"after_monotonic_ns\":%llu}\n",++index,before,now());fflush(stdout);
 }
 key(KEY_LEFTALT,0);key(KEY_LEFTCTRL,0);key(KEY_LEFTMETA,0);wl_display_flush(display);wl_display_disconnect(display);return 0;
}
