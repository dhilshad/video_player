PROJECT_DIR = $(CURDIR)
SRC_FILES := $(wildcard $(PROJECT_DIR)/src/*.c)
INC_FILES := $(wildcard $(PROJECT_DIR)/inc/*.h)
INC_FOLDER := $(PROJECT_DIR)/inc/
TARGET = vd_player

CC = gcc
CFLAGS = `pkg-config --cflags --libs gstreamer-video-1.0 gtk+-3.0 gstreamer-1.0 dbus-1`

.PHONY: all clean

all: $(SRC_FILES)
	@echo "Build starting.."
	$(CC) -o $(TARGET) 

$(TARGET): $(SRC_FILES) $(INC_FILES)
	@echo "Building Video PLayer"
	@echo $(SRC_FILES)
	$(CC) -o $@ $^ -I$(INC_FOLDER) $(CFLAGS)

clean:
	rm $(TARGET)
	
