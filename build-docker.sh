#!/bin/bash
set -e

echo "Building fixture runner..."
cd "$(dirname "$0")"
docker build -t kuksa-fixture-runner:latest .
echo "Fixture runner built: kuksa-fixture-runner:latest"
