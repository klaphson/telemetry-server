.PHONY: configure build run test format hooks clean docker-build docker-shell docker-run

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

test: build
	ctest --test-dir build --output-on-failure

clean:
	rm -rf build

docker-build:
	docker compose build

docker-shell:
	docker compose up -d --build dev
	docker compose exec dev bash

docker-run:
	docker compose run --rm dev bash -lc "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/telemetry-server"
