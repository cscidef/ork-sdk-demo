.PHONY: help setup test test-python test-cpp server-start server-stop demo-echo clean pack-server

# Default target
help:
	@echo "Ork SDK Demo - Available targets:"
	@echo "  make setup          - Set up Python environment"
	@echo "  make pack-server    - Package server from harmonia_ork build"
	@echo "  make server-start   - Start the demo server"
	@echo "  make server-stop    - Stop the demo server"
	@echo "  make test           - Run all tests (Python + C++)"
	@echo "  make test-python    - Run Python tests only"
	@echo "  make test-cpp       - Run C++ tests only"
	@echo "  make demo-echo      - Run the echo demo"
	@echo "  make clean          - Clean build artifacts"

setup:
	cd python && bash setup.sh

pack-server:
	bash scripts/pack_server.sh

server-start:
	bash server/start.sh

server-stop:
	bash server/stop.sh

test-python:
	cd python && .venv/bin/python -m pytest tests/ -v

test-cpp:
	cd cpp && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure

test: test-python test-cpp

demo-echo: server-start
	@echo "Running echo demo..."
	cd python && .venv/bin/python demos/01_echo/client.py
	@$(MAKE) server-stop

clean:
	rm -rf python/.venv python/__pycache__ python/lib/kcp/*.so python/lib/kcp/*.dylib
	rm -rf cpp/build
	rm -f server/.server.pid server/server.log
