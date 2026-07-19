#!/bin/bash
# Multi-node cluster test — 3 containers on a shared network
# node-a: GPU/ARM64, node-b: CPU/AMD64, node-c: CPU/AMD64

NET="socrates-e2e-net"

echo "Creating Docker network: $NET"
docker network create "$NET" 2>/dev/null

echo "=== Node A: GPU + ARM64 ==="
docker run --rm -d --name node-a --network "$NET" --hostname node-a \
  -e NODE_ID=node-a -e NODE_GPU=1 -e NODE_ARCH=arm64 \
  socrates-node

echo "=== Node B: CPU + AMD64 ==="
docker run --rm -d --name node-b --network "$NET" --hostname node-b \
  -e NODE_ID=node-b -e NODE_GPU=0 -e NODE_ARCH=amd64 \
  socrates-node

echo "=== Node C: CPU + AMD64 (likely leader) ==="
docker run --rm -d --name node-c --network "$NET" --hostname node-c \
  -e NODE_ID=node-c -e NODE_GPU=0 -e NODE_ARCH=amd64 \
  socrates-node

echo "Waiting 5s for cluster to converge..."
sleep 5

echo ""
echo "=== Node A logs ==="
docker logs node-a 2>&1

echo ""
echo "=== Node B logs ==="
docker logs node-b 2>&1

echo ""
echo "=== Node C logs ==="
docker logs node-c 2>&1

echo ""
echo "=== Cleanup ==="
docker stop node-a node-b node-c 2>/dev/null
docker network rm "$NET" 2>/dev/null
