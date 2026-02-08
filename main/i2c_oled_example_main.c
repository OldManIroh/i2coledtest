#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "esp_lcd_panel_vendor.h"


static const char *TAG = "example";

#define I2C_BUS_PORT  0  // Номер I2C порта (I2C0)

// Настройки I2C для SSD1306
#define LCD_PIXEL_CLOCK_HZ    (400 * 1000) // Частота I2C: 400 кГц
#define PIN_NUM_SDA           5 // GPIO5 для SDA
#define PIN_NUM_SCL           4 // GPIO4 для SCL
#define PIN_NUM_RST           -1 // RST не используется (-1)
#define I2C_HW_ADDR           0x3C // Адрес SSD1306 на шине I2C


#define LCD_H_RES              128 // Ширина в пикселях
#define LCD_V_RES              64 // Высота в пикселях

// Номер бита, используемый для представления команды и параметра
#define LCD_CMD_BITS           8 // Команды 8-битные
#define LCD_PARAM_BITS         8 // Параметры 8-битные

#define LVGL_TICK_PERIOD_MS    5 // Период обновления LVGL (мс)
#define LVGL_TASK_STACK_SIZE   (4 * 1024) // Память под задачу LVGL
#define LVGL_TASK_PRIORITY     2 // Приоритет задачи LVGL
#define LVGL_PALETTE_SIZE      8 // Размер палитры LVGL (для монохромного)
#define LVGL_TASK_MAX_DELAY_MS 500 // Макс. задержка задачи
#define LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ // Мин. задержка

// Буфер для отображения (128x64/8 = 1024 байта)
// Чтобы использовать LV_COLOR_FORMAT_I1, нам нужен дополнительный буфер для хранения преобразованных данных.
static uint8_t oled_buffer[LCD_H_RES * LCD_V_RES / 8];
// Библиотека LVGL не является потокобезопасной, в этом примере будут вызываться API LVGL из разных задач, поэтому для ее защиты используйте мьютекс.
// Мьютекс для защиты LVGL API (LVGL не потокобезопасен)
static _lock_t lvgl_api_lock;

LV_FONT_DECLARE(Russian);// Объявление русского шрифта 

void lvgl_demo_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp); // Получаем активный экран дисплея
    lv_obj_t *label = lv_label_create(scr); // Создаем объект метки (текст)
    lv_obj_set_style_text_font(label, &Russian, LV_PART_MAIN); // Устанавливаем русский шрифт для метки
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); // Режим циклической прокрутки текста
    lv_label_set_text(label, "Лось\n228");// Устанавливаем текст с переносом строки (\n)
    /* Размер экрана (если вы используете поворот на 90 или 270, используйте lv_display_get_vertical_solve) */
    lv_obj_set_width(label, lv_display_get_horizontal_resolution(disp));// Устанавливаем ширину метки равной ширине дисплея
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);// Выравниваем метку по верхнему центру экрана
}

// Колбэк, вызываемый когда передача данных на дисплей завершена
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;// Получаем указатель на дисплей LVGL
    lv_display_flush_ready(disp);// Сообщаем LVGL, что можно обновлять буфер
    return false;// Не требуется повторный вызов

}

// Основной колбэк отрисовки LVGL
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);// Получаем handle панели из пользовательских данных дисплея

    // Это необходимо, поскольку LVGL резервирует в буфере 2 x 4 байта, поскольку предполагается, что они будут использоваться в качестве палитры. Пропустите палитру здесь
    // Дополнительную информацию о монохромном режиме можно найти по адресу https://docs.lvgl.io/9.2/porting/display.html#monochrome-displays.

    px_map += LVGL_PALETTE_SIZE;// Пропускаем палитру LVGL (первые 8 байт)

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);// Получаем физическое разрешение по горизонтали
    int x1 = area->x1;// Координаты области для обновления
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;
// Преобразуем 1-битные данные LVGL в 8-битный формат SSD1306
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {

            bool chroma_color = (px_map[(hor_res >> 3) * y  + (x >> 3)] & 1 << (7 - x % 8));// Получаем цвет пикселя из буфера LVGL

            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + (x);// Вычисляем позицию в буфере SSD1306
            // Устанавливаем или сбрасываем бит в зависимости от цвета
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));// Пиксель включен
            } else {
                (*buf) |= (1 << (y % 8));// Пиксель выключен
            }
        }
    }
    // передать буфер отрисовки драйверу
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);// Пиксель выключен
}

// Колбэк таймера для обновления внутренних часов LVGL
static void increase_lvgl_tick(void *arg)
{
    /* Сообщите LVGL, сколько миллисекунд прошло. */
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}
// Основная задача обработки LVGL
static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);// Захватываем мьютекс перед вызовом LVGL API
        time_till_next_ms = lv_timer_handler();// Выполняем обработку таймеров LVGL
        _lock_release(&lvgl_api_lock);
        // в случае срабатывания тайм-аута сторожевого таймера задачи
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);// Ограничиваем время задержки для стабильности
        // в случае, если отображение lvgl еще не готово
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);// Задержка до следующего обновления
    }
}

void app_main(void)

{
    

    //Инициализация I2C

    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = PIN_NUM_SDA,
        .scl_io_num = PIN_NUM_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    //Интерфейс панели I2C
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = I2C_HW_ADDR,    // Адрес устройства
        .scl_speed_hz = LCD_PIXEL_CLOCK_HZ, // Скорость
        .control_phase_bytes = 1,    // Байт управления           // Согласно паспорту SSD1306.
        .lcd_cmd_bits = LCD_CMD_BITS, // Разрядность команд  // Согласно паспорту SSD1306.
        .lcd_param_bits = LCD_CMD_BITS, // Разрядность параметров // Согласно паспорту SSD1306.
        .dc_bit_offset = 6,             // Бит выбора команда/данные        // Согласно паспорту SSD1306.

    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));


    // Драйвер SSD1306
    ESP_LOGI(TAG, "Install SSD1306 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,// 1-битный (монохромный)
        .reset_gpio_num = PIN_NUM_RST, // Пин сброса
    };

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = LCD_V_RES, // Высота дисплея
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));


    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle)); // Аппаратный сброс
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle)); // Инициализация
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true)); // Включение

    // Инициализация LVGL
    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();// Инициализация библиотеки LVGL

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);// Создание дисплея LVGL с указанным разрешением

    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true)); // Зеркальное отображение (опционально, зависит от монтажа дисплея)
    // связать дескриптор панели i2c с дисплеем
    lv_display_set_user_data(display, panel_handle);// Связывание панели с дисплеем LVGL
    
    //Буферы отрисовки LVGL
    void *buf = NULL;
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers");
// Расчет размера буфера: (128*64/8) + 8 байт палитры = 1032 байта
    size_t draw_buffer_sz = LCD_H_RES * LCD_V_RES / 8 + LVGL_PALETTE_SIZE;
    buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);// Выделение памяти в внутренней RAM
    assert(buf);// Проверка выделения

    // Настройка формата цвета (1-битный монохромный).
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    // Установка буферов отрисовки
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    // Установка колбэка отрисовки
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    //Регистрация колбэков панели
    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    /* Зарегистрировать выполненный обратный вызов */
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

    // Таймер для LVGL
    ESP_LOGI(TAG, "Use esp_timer as LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    //Задача LVGL
    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    // Создание пользовательского интерфейса
    ESP_LOGI(TAG, "Display LVGL Scroll Text");
    // Блокируйте мьютекс, поскольку API-интерфейсы LVGL не являются потокобезопасными.
    _lock_acquire(&lvgl_api_lock);// Захват мьютекса
    lvgl_demo_ui(display);// Создание UI
    _lock_release(&lvgl_api_lock);// Освобождение мьютекса
}