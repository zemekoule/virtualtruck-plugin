SDK_INCLUDE=scs_sdk_1_16/include
DATA_DIR=$(abspath ../data)
PLUGINS_DIR=$(HOME)/Library/Application Support/Steam/steamapps/common/American Truck Simulator/American Truck Simulator.app/Contents/MacOS/plugins

# The game binary is x86_64 only (runs under Rosetta on Apple Silicon).
virtualtruck.so: virtualtruck.cpp
	clang++ -arch x86_64 -std=c++17 -O2 -Wall -fPIC -shared -Wl,-install_name,$@ -I$(SDK_INCLUDE) -DVIRTUALTRUCK_DATA_DIR='"$(DATA_DIR)"' -o $@ $<

install: virtualtruck.so
	cp virtualtruck.so "$(PLUGINS_DIR)/"

uninstall:
	rm -f "$(PLUGINS_DIR)/virtualtruck.so"

clean:
	rm -f virtualtruck.so

.PHONY: install uninstall clean
