

# ESP32 Music Player with Button Control

## Project Overview
This project involves creating a music player on an **ESP32** using PlatformIO. The music player plays two audio files stored on an SD card. The playback is controlled via two physical buttons, allowing users to toggle between playing the main music and the background music. Additionally, both tracks can be played simultaneously with mixing functionality when required.
Chức năng chính:

Phát nhạc: Đọc và phát file gong.wav từ thẻ SD qua I2S khi nhấn nút GPIO_BUTTON.
Mix âm thanh: Mix file huh.wav khi nhấn nút GPIO_BUTTON_1.
Bật/tắt: Nhấn lại GPIO_BUTTON để dừng nhạc.
Đầu ra: Âm thanh được phát qua giao thức I2S (giả sử kết nối với loa hoặc DAC).
Cấu trúc:

Task:
audio_processing_task: Đọc file WAV, mix âm thanh, gửi dữ liệu vào queue.
i2s_output_task: Lấy dữ liệu từ queue và phát qua I2S.
button_task: Xử lý sự kiện nút bấm qua ISR.
FreeRTOS Components:
EventGroup: Quản lý trạng thái (phát nhạc, mixing, dừng).
Queue: Truyền dữ liệu âm thanh giữa các task.
Timer: Debounce nút bấm để tránh nhiễu.
ISR: Xử lý ngắt từ nút bấm.
Thư viện:
ESP-IDF: esp_err.h, esp_log.h, driver/i2s.h, driver/gpio.h.
FreeRTOS: freertos/task.h, freertos/queue.h, freertos/event_groups.h, freertos/timers.h.
Tùy chỉnh: I2SOutput.h, SDCard.h, SPIFFS.h, WAVFileReader.h, config.h.
Đặc điểm:
Tận dụng FreeRTOS: Sử dụng task priority, EventGroup, Queue, Timer, ISR để tối ưu hiệu suất và đồng bộ hóa.
Đầu ra I2S: Dữ liệu âm thanh được gửi qua I2S, phù hợp với loa hoặc DAC.

## Project Components

1. **ESP32 Development Board**: The core hardware running the application.
2. **SD Card**: Used to store the audio files (WAV format). The files are read and played via I2S.
3. **I2S Interface**: Used to send audio data to the audio output device (DAC or speaker).
4. **Buttons (GPIO_BUTTON and GPIO_BUTTON_1)**: Two physical buttons are used to control the music playback:
    - **GPIO_BUTTON**: Toggles the main music playback (main_music.wav).
    - **GPIO_BUTTON_1**: Plays background music (background_music_1.wav) while the main music is playing.
5. **Speaker/Output**: The audio is output through I2S to a connected speaker or DAC.

## Functionality

- **Button Control**: 
  - **GPIO_BUTTON**:
    - When pressed, the **main_music.wav** will start playing.
    - When pressed again, **main_music.wav** will stop.
  - **GPIO_BUTTON_1**:
    - When pressed while **main_music.wav** is playing, **background_music_1.wav** will start playing simultaneously.
    - If **background_music_1.wav** is already playing, it will stop.
  
- **Audio Files**:
  - **main_music.wav**: The primary music track that can be toggled on or off.
  - **background_music_1.wav**: The secondary music track that plays in the background along with **main_music.wav**.

## Task and Task Scheduling

### Tasks
This project has three main tasks that run concurrently:

1. **`handle_button_push`** (Button Handler Task)
    - **Function**: This is the main task responsible for monitoring the button presses. When **GPIO_BUTTON** is pressed, it toggles the playback of **main_music.wav**. When **GPIO_BUTTON_1** is pressed, it toggles the playback of **background_music_1.wav** while ensuring **main_music.wav** is already playing.
    - **Priority**: High (1)
    - **Schedule**: This task runs in a continuous loop, checking the state of the buttons and taking appropriate actions based on the button presses.

2. **`main_music_task`** (Main Music Playback Task)
    - **Function**: This task is responsible for playing **main_music.wav**. It continuously plays the music until **GPIO_BUTTON** is pressed again to stop the playback.
    - **Priority**: Medium (2)
    - **Schedule**: This task is created and runs when **main_music.wav** is supposed to play. It is deleted when **main_music.wav** stops.

3. **`background_music_task`** (Background Music Playback Task)
    - **Function**: This task plays **background_music_1.wav** while **main_music.wav** is already playing. When **GPIO_BUTTON_1** is pressed, this task starts the background music.
    - **Priority**: Medium (2)
    - **Schedule**: This task runs only when **main_music.wav** is playing and **GPIO_BUTTON_1** is pressed. It is deleted when background music is no longer required.

### Task Scheduling and Execution Flow

1. **Main Task (`handle_button_push`)**:
   - The **`handle_button_push`** task runs continuously, checking the state of the buttons. When a button is pressed, it triggers actions in the other tasks (e.g., playing or stopping music).
   
2. **Main Music Task (`main_music_task`)**:
   - The **`main_music_task`** runs when **main_music.wav** needs to be played. It continues running until the task is deleted (i.e., **main_music.wav** stops).

3. **Background Music Task (`background_music_task`)**:
   - The **`background_music_task`** runs when **background_music_1.wav** is played alongside **main_music.wav**. It only runs when **main_music.wav** is playing, and it stops when **background_music_1.wav** is no longer needed.

### Task Execution Flow

1. The **`handle_button_push`** task listens for button presses.
2. When **`GPIO_BUTTON`** is pressed:
   - If **main_music.wav** is not playing, it starts playing the music.
   - If **main_music.wav** is playing, it stops the music.
3. When **`GPIO_BUTTON_1`** is pressed:
   - If **main_music.wav** is already playing, it starts **background_music_1.wav**.
   - If **background_music_1.wav** is already playing, it stops the background music.

### Task Priority

- **`handle_button_push`**: High priority (1) since it controls the entire flow of music playback.
- **`main_music_task` and `background_music_task`**: Medium priority (2) because they handle audio playback and are dependent on user input via **`handle_button_push`**.

## Project Setup

### Prerequisites
1. Install **PlatformIO** and **Visual Studio Code**.
2. Configure **PlatformIO** for **ESP32**.
3. Connect an **SD card** to your ESP32 for storing the audio files.
4. Connect your **I2S speaker** or **DAC** to the appropriate ESP32 pins.

### File Organization
- **`src/`**: Contains the main source code, including `main.cpp` where the program logic is implemented.
- **`platformio.ini`**: Configuration file for PlatformIO, specifying the board, framework, and libraries.

### How to Run
1. Clone this repository or download the project files.
2. Open the project in **VS Code** with **PlatformIO**.
3. Ensure the **PlatformIO** environment is set up correctly for **ESP32**.
4. Replace the audio files **`main_music.wav`** and **`background_music_1.wav`** in the **`/sdcard`** folder.
5. Connect the ESP32 to your computer and upload the code via PlatformIO.
6. Open the **serial monitor** to observe the output and debug information.

### File Structure
```
/lib              # Directory for ex-lib
  ├── audio_output    
  ├── wav_file
  ├── spiffs     
  └── sd_card

/src
  ├── config.h
  ├── config.cpp  
  ├── CMakeLists.txt  
  └── main.cpp          # Main logic for handling music playback and button input
/platformio.ini         # PlatformIO configuration file
/sdcard                 # Directory for storing audio files
  ├── main_music.wav    # Main music file to be played
  └── background_music_1.wav # Background music file
```

## Conclusion
This project demonstrates how to control audio playback on an **ESP32** using two buttons. The project features **simple task management**, with tasks for handling button input, playing main and background music, and controlling task execution using **FreeRTOS**. The code allows for flexible audio playback control and can be expanded to handle additional features or audio files.

---

### Additional Notes:
1. **Task management** in FreeRTOS ensures efficient handling of multiple tasks, where the task that listens to button presses has the highest priority to make sure user inputs are processed immediately.
2. **I2S** is used to interface with the speaker or DAC for audio output, ensuring high-quality sound playback.
