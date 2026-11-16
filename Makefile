# AgentForge — сборка. Ноль внешних зависимостей (только stdlib + dl).
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -ldl -pthread
BUILD := build

SRC := src/json.cpp src/scene.cpp src/render.cpp src/assets.cpp src/sim.cpp \
       src/agent.cpp src/selftest.cpp src/benchmark.cpp src/main.cpp
OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))

all: $(BUILD)/afengine

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Iinclude -c $< -o $@

$(BUILD)/afengine: $(OBJ)
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ $(OBJ) $(LDFLAGS)

# Пример плагина игровой логики (C ABI) — json.cpp нужен плагину (Json::parse/dump)
plugin: $(BUILD)/game_logic.so
$(BUILD)/game_logic.so: game/game_logic.cpp src/json.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Iinclude -shared -o $@ game/game_logic.cpp src/json.cpp $(LDFLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean plugin
