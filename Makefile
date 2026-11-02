.PHONY: configure build run run-service test benchmark-build benchmark benchmark-matrix benchmark-test benchmark-ipc thread-lab thread-lab-test thread-lab-tsan profile-syscalls profile-cpu-build profile-cpu profile-cpu-check profile-cpu-test format hooks clean docker-build docker-shell docker-run

format:
	pre-commit run --all-files

hooks:
	pre-commit install

configure:
	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build build

run: build
	./build/telemetry-server

run-service: build
	runuser -u telemetry -- \
		./build/telemetry-server \
		--data-file /var/lib/telemetry-server/telemetry.bin

test: build
	ctest --test-dir build --output-on-failure

benchmark-build:
	cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTELEMETRY_BUILD_BENCHMARKS=ON
	cmake --build build-bench

benchmark: benchmark-build
	bash bench/run_benchmark.sh

benchmark-matrix: benchmark-build
	bash bench/run_benchmark.sh 500000 128 19000 --batches 1,32,128,512 --repeat 3

benchmark-test:
	python3 -m unittest discover -s bench -p 'test_*.py'

benchmark-ipc: benchmark-build
	mkdir -p build-ipc-results
	bash bench/run_benchmark.sh 1000000 128 19000 --repeat 5 --csv build-ipc-results/batched.csv

# Educational two-thread producer/consumer exercise (no TCP or disk I/O).
thread-lab: benchmark-build
	./build-bench/telemetry-thread-lab --frames 1000000 --batch 128 --repeat 5

thread-lab-test: benchmark-build
	./build-bench/telemetry-thread-lab --self-test

# The container kernel may restrict ThreadSanitizer; use only in development.
thread-lab-tsan:
	mkdir -p build-bench
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread -fsanitize=thread -fno-omit-frame-pointer bench/thread_sync_lab.cpp -o build-bench/telemetry-thread-lab-tsan
	TSAN_OPTIONS=halt_on_error=1 ./build-bench/telemetry-thread-lab-tsan --self-test

profile-syscalls: benchmark-build
	python3 bench/profile_syscalls.py --frames 10000 --batch 128 --port 19000

profile-cpu-build:
	cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTELEMETRY_BUILD_BENCHMARKS=ON -DTELEMETRY_PROFILE_CPU=ON
	cmake --build build-perf

profile-cpu-test: profile-cpu-build
	./build-perf/telemetry-profile-cpu --self-test

profile-cpu-check: profile-cpu-build
	./build-perf/telemetry-profile-cpu --check

profile-cpu: profile-cpu-build
	./build-perf/telemetry-profile-cpu --frames 1000000 --batch 128 --repeat 20

clean:
	rm -rf build

docker-build:
	docker compose build

docker-shell:
	docker compose up -d --build dev
	docker compose exec dev bash

docker-run:
	docker compose run --rm dev bash -lc "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/telemetry-server"
