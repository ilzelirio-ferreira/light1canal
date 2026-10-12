"""Compile the real relay controller against fake hardware and a stalled Matter stack."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
common = r'''#pragma once
#include <cstdint>
#include <cassert>
#include <functional>
using esp_err_t=int; constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_NOT_FOUND=3, ESP_ERR_INVALID_STATE=4, ESP_ERR_NO_MEM=5;
#define ESP_ERR_NOT_SUPPORTED 6
#define CONFIG_IDF_TARGET_ESP32S2 1
#define ESP_RETURN_ON_ERROR(expr,...) do {int e=(expr);if(e)return e;} while(0)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGE(...) ((void)TAG)
#define ESP_LOGW(...) ((void)TAG)
inline const char* esp_err_to_name(int){return "error";}
using gpio_num_t=int;using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
constexpr int GPIO_MODE_OUTPUT=1,GPIO_MODE_INPUT=2,GPIO_PULLUP_ENABLE=1,GPIO_INTR_NEGEDGE=1;
inline bool GPIO_IS_VALID_OUTPUT_GPIO(int){return true;} inline bool GPIO_IS_VALID_GPIO(int){return true;}
struct gpio_config_t{int mode=0,pull_up_en=0;uint64_t pin_bit_mask=0;};
static int physical_level=0;
inline int gpio_set_level(int,bool b){physical_level=b;return 0;}
inline int gpio_config(gpio_config_t*){return 0;}
inline int gpio_install_isr_service(int){return 0;}
inline int gpio_isr_handler_add(int,void(*)(void*),void*){return 0;}
inline int gpio_set_intr_type(int,int){return 0;}
static int64_t fake_time=0;
inline int64_t esp_timer_get_time(){return fake_time;}
using TaskHandle_t=void*;
inline TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<void*>(1);}
constexpr int pdPASS=1;
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(int){}
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){return pdPASS;}
constexpr int CHIP_NO_ERROR=0;
struct esp_matter_attr_val_t{union{bool b;uint8_t u8;}val;};
inline esp_matter_attr_val_t esp_matter_bool(bool b){return {{b}};}
inline esp_matter_attr_val_t esp_matter_nullable_uint8(uint8_t n){esp_matter_attr_val_t v{};v.val.u8=n;return v;}
static uint8_t reported_brightness=0;
static bool reported=false;static std::function<void()> during_report;
namespace chip { namespace app {namespace Clusters {namespace OnOff {constexpr int Id=6;namespace Attributes{namespace OnOff{constexpr int Id=0;}}}namespace LevelControl{constexpr int Id=8;namespace Attributes{namespace CurrentLevel{constexpr int Id=0;}}}}}
namespace DeviceLayer {struct Manager{int ScheduleWork(void(*)(intptr_t),intptr_t){return 1;}};inline Manager& PlatformMgr(){static Manager m;return m;}}}
namespace esp_matter {namespace attribute {int update(uint16_t,int,int,esp_matter_attr_val_t*);}}
'''
test = r'''#include "app_relay.cpp"
namespace esp_matter {namespace attribute {
int update(uint16_t endpoint,int cluster,int,esp_matter_attr_val_t* value){
 if(cluster==chip::app::Clusters::LevelControl::Id){reported_brightness=value->val.u8;return app_relay_set_brightness(endpoint,value->val.u8);}
 reported=value->val.b;
 if(during_report)during_report();
 return app_relay_set_power(endpoint,value->val.b);
}}}
int main(){
 assert(app_relay_init()==ESP_OK);
 assert(app_relay_start_switch()==ESP_OK); // No Matter endpoint or network exists yet.
 assert(toggle_local(0)==ESP_OK && physical_level==1);
 assert(toggle_local(0)==ESP_OK && physical_level==0);
 assert(app_relay_register_endpoint(0,1)==ESP_OK);
 assert(app_relay_restore_power(1,true)==ESP_OK && physical_level==0); // Don't undo boot-time switch operations.
 assert(app_relay_set_power(1,true)==ESP_OK && physical_level==0); // Startup callbacks also preserve physical operations.
 app_relay_enable_matter_sync();
 queue_sync(0); // Fake Matter queue rejects the work.
 assert(!pending[0] && dirty[0] && physical_level==0);
 assert(toggle_local(0)==ESP_OK && physical_level==1);
 during_report=[](){assert(toggle_local(0)==ESP_OK);}; // Local input changes during an old Matter report.
 sync_matter(0);
 assert(physical_level==0 && reported==true && dirty[0]); // Stale report cannot restore old hardware level.
 during_report={};sync_matter(0);
 assert(!reported && physical_level==0 && !dirty[0]);
 assert(app_relay_set_power(1,true)==ESP_OK && physical_level==1); // Remote control still works.
 assert(toggle_local(0)==ESP_OK && physical_level==0); // Physical input toggles the actual remote state.
 assert(app_relay_set_power(1,true)==ESP_OK);
 assert(blink[0].level==254 && physical_level==1);
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,1)==ESP_OK);
 assert(AppBlink::half_period(1)==500000);
 fake_time=499999;blink_tick(0,fake_time);assert(physical_level==1);
 fake_time=500000;blink_tick(0,fake_time);assert(physical_level==0 && power[0]);
 fake_time=1000000;blink_tick(0,fake_time);assert(physical_level==1);
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,253)==ESP_OK);
 assert(AppBlink::half_period(253)==50000);
 fake_time+=50000;blink_tick(0,fake_time);assert(physical_level==0);
 assert(app_relay_power_command(1,true)==ESP_OK); // Explicit On cancels flashing even if logically on.
 fake_time+=1000000;blink_tick(0,fake_time);assert(physical_level==1 && blink[0].level==254);
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,253)==ESP_OK);
 fake_time+=50000;blink_tick(0,fake_time);assert(physical_level==0 && power[0]);
 assert(toggle_local(0)==ESP_OK && !power[0] && blink[0].level==0);
 fake_time+=1000000;blink_tick(0,fake_time);assert(physical_level==0);
 assert(toggle_local(0)==ESP_OK && physical_level==1 && blink[0].level==254);
 sync_matter(0);assert(reported && reported_brightness==254);
 for(int n=2;n<=253;++n)assert(AppBlink::half_period(n)<=AppBlink::half_period(n-1));
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,0)==ESP_OK && blink[0].level==0 && !power[0] && physical_level==0);
 assert(app_relay_power_command(1,false)==ESP_OK && physical_level==0 && blink[0].level==0);
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,254)==ESP_OK && physical_level==1 && blink[0].level==254);
 fake_time+=1000000;blink_tick(0,fake_time);assert(physical_level==1);
 assert(app_relay_power_command(1,true)==ESP_OK && blink[0].level==254);
 assert(app_relay_power_command(1,false)==ESP_OK && blink[0].level==0);
 // Reproduce the SDK restoring the old brightness after an Off command.
 assert(app_relay_set_brightness(1,128)==ESP_OK && physical_level==0 && !power[0]);
 fake_time+=1000000;blink_tick(0,fake_time);assert(physical_level==0);
 sync_matter(0);assert(!reported && reported_brightness==0);
 assert(app_relay_power_command(1,true)==ESP_OK);
 assert(app_relay_set_brightness(1,128)==ESP_OK && physical_level==1 && blink[0].level==254);
 assert(app_relay_brightness_command(1)==ESP_OK);
 assert(app_relay_set_brightness(1,128)==ESP_OK && blink[0].level==128);
 assert(app_relay_set_power(1,true)==ESP_OK && blink[0].level==128); // Coupled OnOff keeps requested frequency.
 assert(app_relay_power_command(1,false)==ESP_OK);
 assert(app_relay_set_brightness(1,254)==ESP_OK && !power[0]);
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=pathlib.Path(directory)
 (d/'fake.h').write_text(common)
 for name in ['sdkconfig.h','esp_err.h','driver/gpio.h','esp_check.h','esp_matter.h','esp_timer.h','freertos/FreeRTOS.h','freertos/task.h','platform/CHIPDeviceLayer.h']:
  f=d/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#include "fake.h"\n')
 (d/'test.cpp').write_text(test)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(d),'-I'+str(root/'light/main'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
 print('Local GPIO: no Matter startup, failed queue, stale report and remote control passed.')
