# Docker Compose HAProxy 503 Error Diagnosis and Solutions

## Problem Summary
You are experiencing a 503 Service Unavailable error when accessing port 8080, with HAProxy logs showing:
```
cache-loadbalancer  | [WARNING]  (8) : Server cache_backend/cache-node1 is DOWN, reason: Layer4 connection problem, info: "Connection refused", check duration: 0ms. 2 active and 0 backup servers left. 0 sessions active, 0 requeued, 0 remaining in queue.
cache-loadbalancer  | [ALERT]    (8) : backend 'cache_backend' has no server available!
```

## Root Cause Analysis

### 1. Cache Nodes Not Starting
Based on your Docker Compose configuration and error logs, the three cache nodes (`cache-node1`, `cache-node2`, `cache-node3`) are not starting successfully, which causes HAProxy to have no available backends.

### 2. Potential Issues Identified

#### A. Missing Executable
The Docker Compose configuration tries to run `/opt/cache/bin/cache_server`, but this executable likely doesn't exist in the container because:
- The Dockerfile builds a C++ application but may not copy it to the expected location
- The binary might not be executable or have the wrong permissions

#### B. Configuration File Issues
Each node expects a configuration file at `/opt/cache/config/cache_config.yaml`, but:
- The actual configuration files are node-specific (`node1.yaml`, `node2.yaml`, `node3.yaml`)
- The directory structure might not exist in the container

#### C. Directory Structure Missing
The configuration expects directories like:
- `/var/lib/cache/data` for data storage
- `/var/lib/cache/wal` for Write-Ahead Logging
- `/opt/cache/logs` for logging

#### D. SSL Certificate Issues
The configuration references SSL certificates that probably don't exist:
- `/etc/ssl/certs/cache-server.crt`
- `/etc/ssl/private/cache-server.key`

## Solutions

### Solution 1: Check Container Build and Logs

First, let's examine what's happening with the containers:

```bash
# Check if containers are being created
sudo docker-compose ps

# Check the logs of individual services
sudo docker-compose logs cache-node1
sudo docker-compose logs cache-node2 
sudo docker-compose logs cache-node3
sudo docker-compose logs loadbalancer

# Try to build and start one service at a time
sudo docker-compose up cache-node1
```

### Solution 2: Fix Dockerfile and Binary Location

Ensure your `Dockerfile` correctly builds and places the cache server binary:

```dockerfile
# Add to your Dockerfile
COPY --from=builder /path/to/built/cache_server /opt/cache/bin/cache_server
RUN chmod +x /opt/cache/bin/cache_server

# Create required directories
RUN mkdir -p /opt/cache/config /opt/cache/logs /opt/cache/data \
    /var/lib/cache/data /var/lib/cache/wal /etc/ssl/certs /etc/ssl/private
```

### Solution 3: Fix Configuration File Mapping

Update the volume mappings in `docker-compose.yml`:

```yaml
# For cache-node1
volumes:
  - cache-node1-data:/var/lib/cache/data  # Changed from /opt/cache/data
  - cache-node1-logs:/opt/cache/logs
  - ./config/node1.yaml:/opt/cache/config/cache_config.yaml
```

### Solution 4: Simplify SSL Configuration

For development/testing, modify the node configuration files to disable SSL:

```yaml
# In config/node1.yaml, node2.yaml, node3.yaml
network:
  listen_address: "0.0.0.0"
  listen_port: 8081  # or respective ports
  enable_ssl: false  # Changed from true
  # Comment out SSL certificate lines
  # ssl_cert_file: "/etc/ssl/certs/cache-server.crt"
  # ssl_key_file: "/etc/ssl/private/cache-server.key"
```

### Solution 5: Fix HAProxy Health Checks

In `config/haproxy.cfg`, temporarily disable health checks:

```haproxy
backend cache_backend
    balance roundrobin
    # Comment out health check temporarily
    # option httpchk GET /health
    
    server cache-node1 cache-node1:8081
    server cache-node2 cache-node2:8082  
    server cache-node3 cache-node3:8083
```

### Solution 6: Step-by-Step Debugging Process

1. **Build and test one component at a time:**
```bash
# Build the application first
sudo docker-compose build cache-node1

# Start only the first cache node
sudo docker-compose up cache-node1

# In another terminal, check if it's responding
sudo docker exec -it cache-node1 /bin/sh
# or
curl http://localhost:8081/
```

2. **Check network connectivity:**
```bash
# Test internal Docker network connectivity
sudo docker-compose up cache-node1 cache-node2
sudo docker-compose exec cache-node2 ping cache-node1
```

3. **Gradually add services:**
```bash
# Start cache nodes first
sudo docker-compose up cache-node1 cache-node2 cache-node3

# Then add load balancer
sudo docker-compose up loadbalancer
```

### Solution 7: Alternative Simple Test Configuration

Create a minimal test setup to verify basic connectivity:

1. **Temporary simple HAProxy config** (`config/haproxy-simple.cfg`):
```haproxy
global
    daemon

defaults
    mode tcp
    timeout connect 5000ms
    timeout client 50000ms
    timeout server 50000ms

frontend cache_frontend
    bind *:8080
    default_backend cache_backend

backend cache_backend
    balance roundrobin
    server cache-node1 cache-node1:8081 check inter 30s fall 3 rise 2
    server cache-node2 cache-node2:8082 check inter 30s fall 3 rise 2
    server cache-node3 cache-node3:8083 check inter 30s fall 3 rise 2
```

2. **Temporary simple cache node config** (disable complex features):
```yaml
node:
  node_id: "cache-node-001"

network:
  listen_address: "0.0.0.0"
  listen_port: 8081
  enable_ssl: false

storage:
  engine_type: "memory"  # Use simpler in-memory storage for testing
  data_directory: "/tmp/cache"
```

## Recommended Debugging Steps

1. **Check container status:**
```bash
sudo docker-compose ps
sudo docker-compose logs
```

2. **Verify the cache server binary exists:**
```bash
sudo docker-compose run cache-node1 ls -la /opt/cache/bin/
```

3. **Test configuration files:**
```bash
sudo docker-compose run cache-node1 cat /opt/cache/config/cache_config.yaml
```

4. **Check port availability:**
```bash
sudo docker-compose run cache-node1 netstat -tulpn
```

5. **Manual testing:**
```bash
# Start cache node manually for debugging
sudo docker-compose run cache-node1 /opt/cache/bin/cache_server --config /opt/cache/config/cache_config.yaml --node-id node1 --port 8081 --data-dir /tmp/cache
```

## Next Steps

1. Start with Solution 1 to gather more diagnostic information
2. Fix the Dockerfile and configuration files based on the findings
3. Test each component individually before running the full stack
4. Gradually add complexity back once basic connectivity is working

Run these commands to begin diagnosis:
```bash
sudo docker-compose down
sudo docker-compose build
sudo docker-compose up cache-node1
```