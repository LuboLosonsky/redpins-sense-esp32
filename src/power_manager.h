#pragma once

#include <stdbool.h>
#include <stdint.h>

// Napájacie profily zariadenia. Perzistentné v NVS ("power_cfg"/"power_mode"),
// prepínané cez options menu na obrazovke SYSTEM.
enum PowerMode {
    MODE_PERFORMANCE = 0,  // Plný jas/refresh, žiadne úspory (predvolené)
    MODE_BALANCED = 1,     // Stály nízky jas + pomalší refresh, docasny wake
    MODE_LONG_LIFE = 2     // Deep sleep medzi krátkymi prebudeniami
};

// Načíta uložený režim z NVS a zistí príčinu prebudenia. Volať hneď po
// nvs_flash_init(), pred storage_init().
void power_manager_init(void);

// true, ak ide o rychle prebudenie z MODE_LONG_LIFE deep sleep (nie
// studeny start) - main.cpp podla toho preskakuje BLE/WiFi/weather/boot
// animaciu.
bool power_manager_is_fast_wake(void);

// Volat hned po power_manager_init(), PRED akoukolvek dalsou inicializaciou
// (storage/display/senzory/BLE/WiFi). Vrati true, ak ma boot pokracovat
// normalne (studeny start, iny rezim ako Long Life, alebo Long Life
// fast-wake so SKUTOCNE drzanym tlacidlom). Vrati false len pri rutinnom
// periodickom MODE_LONG_LIFE wake bez drzaneho tlacidla - volajuci by v tom
// pripade mal vykonat pripadnu udrzbu dat (sensor log/weather fetch) a potom
// zavolat power_manager_force_long_life_sleep().
bool power_manager_check_button_wake(void);

// Aplikuje pociatocny stav displeja podla ulozeneho rezimu. Volat raz,
// hned po uspesnom display_hal_init(), pred spustenim gui_task.
void power_manager_on_display_ready(void);

PowerMode power_manager_get_mode(void);

// Zapise novy rezim do NVS a okamzite aplikuje jeho zobrazenie. Volane z
// Settings menu (gui.cpp).
void power_manager_set_mode(PowerMode mode);

// Oznami aktivitu (stlacenie tlacidla, alebo detekcia tiena na LDR). V
// MODE_BALANCED docasne prepne na plny vykon na 60s. Volat z gui.cpp pri
// kazdej hrane stlacenia tlacidla.
void power_manager_notify_activity(void);

// Volat kazdych ~50ms z hlavnej slucky gui_task. Rieši LDR diferencialnu
// detekciu (MODE_BALANCED) a expiraciu docasneho vykonnostneho okna.
void power_manager_tick(void);

// Okamzity a bezpodmienecny prechod do deep sleep (rovnaka logika ako
// MODE_LONG_LIFE), nezavisle od aktualne zvoleneho rezimu. Pouzite pre BLE
// prikaz 0x03 DEEP_SLEEP. Nikdy sa nevrati.
void power_manager_force_long_life_sleep(void);
