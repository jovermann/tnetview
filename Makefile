CXX ?= c++
CXXSTD ?= -std=c++23
BUILD ?= release
CXXFLAGS_COMMON ?= -Wall -Wextra -pedantic
CXXFLAGS_DEBUG ?= -O0 -g
CXXFLAGS_RELEASE ?= -O3 -DNDEBUG
CXXFLAGS := $(CXXFLAGS_COMMON) $(if $(filter debug,$(BUILD)),$(CXXFLAGS_DEBUG),$(CXXFLAGS_RELEASE))

TARGET := tnetview
SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(SOURCES:src/%.cpp=build-$(BUILD)/%.o)

default: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $^ -o $@

build-$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXSTD) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf build-* $(TARGET)

test: $(TARGET)
	./$(TARGET) --once >/dev/null
	./$(TARGET) --help >/dev/null
	./$(TARGET) --colors >/dev/null

.PHONY: default clean test
