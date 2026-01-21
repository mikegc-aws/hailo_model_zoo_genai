# Hailo-Ollama Server Start Procedure

## Server Configuration

- **Host**: 0.0.0.0 (all interfaces)
- **Port**: 8000
- **Config file**: `~/hailo_model_zoo_genai/config/hailo-ollama.json`
- **Build directory**: `~/hailo_model_zoo_genai/build/src/apps/server/`
- **Binary**: `hailo-ollama`

## Quick Start (Recommended)

```bash
# SSH to the server
ssh stranded

# Stop current server (force kill to ensure it stops)
pkill -9 hailo-ollama
sleep 2

# Navigate to build directory
cd ~/hailo_model_zoo_genai/build/src/apps/server

# Run the latest server in background
nohup ./hailo-ollama > /tmp/hailo-ollama.log 2>&1 &

# Verify it's running
sleep 2
ps aux | grep hailo-ollama | grep -v grep

# Verify port 8000 is listening
ss -tlnp | grep 8000

# Check logs
tail -f /tmp/hailo-ollama.log
```

## Full Build and Deploy Procedure

If you've made code changes and need to rebuild:

```bash
# SSH to server
ssh stranded

# Navigate to project
cd ~/hailo_model_zoo_genai

# Stop current server
pkill hailo-ollama

# Pull latest changes (if needed)
git pull

# Build
cd build
cmake --build .

# Run the newly built server
cd src/apps/server
nohup ./hailo-ollama > /tmp/hailo-ollama.log 2>&1 &

# Verify
ps aux | grep hailo-ollama | grep -v grep
```

## Stopping the Server

```bash
# Force kill by name (RECOMMENDED - ensures it stops)
pkill -9 hailo-ollama

# Wait for process to die and port to be released
sleep 2

# Verify it's stopped
ps aux | grep hailo-ollama | grep -v grep || echo "Server stopped"
ss -tlnp | grep 8000 || echo "Port 8000 is free"
```

Note: Regular `pkill hailo-ollama` may not always work. Use `pkill -9` to force kill.

## Checking Server Status

```bash
# Check if process is running
ps aux | grep hailo-ollama | grep -v grep

# Check what's listening on port 8000
ss -tlnp | grep 8000

# Test API endpoint
curl http://localhost:8000/api/tags

# Check logs
tail -f /tmp/hailo-ollama.log
```

## Binary Locations

1. **Primary (latest build)**: `~/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama`
   - This is where cmake builds the binary
   - Always use this after rebuilding

2. **System binary**: `/usr/bin/hailo-ollama`
   - System-wide installation (older version from Nov 7)
   - Not recommended for development

3. **User binary**: `~/.local/bin/hailo-ollama`
   - Can be copied here for convenience
   - Check modification date to ensure it's current

## Configuration

The server reads configuration from `config/hailo-ollama.json`:

```json
{
    "server": {
        "host": "0.0.0.0",
        "port": 8000
    },
    "library": {
        "host": "dev-public.hailo.ai",
        "port": 443
    },
    "main_poll_time_ms": 200
}
```

## Important Notes

- The server automatically reads the config file from the project directory
- Port 8000 is hardcoded in the config
- The binary tries to bind to the port immediately on startup
- Only one instance can run at a time on the same port
- Logs go to `/tmp/hailo-ollama.log` when using nohup
- The server requires Hailo hardware/drivers to load models

## Troubleshooting

**"Address already in use" error**:
```bash
# Find and kill the existing process
ps aux | grep hailo-ollama | grep -v grep
pkill hailo-ollama
# Wait a few seconds, then restart
```

**Binary not found**:
```bash
# Make sure you're in the right directory
cd ~/hailo_model_zoo_genai/build/src/apps/server
ls -lh hailo-ollama
```

**Server not responding**:
```bash
# Check if process is alive
ps aux | grep hailo-ollama

# Check logs for errors
tail -100 /tmp/hailo-ollama.log

# Check if port is listening
ss -tlnp | grep 8000
```

## After Updates

When deploying updated code to the server:

1. Copy updated source files to the server (via scp or git pull)
2. Stop the running server
3. Rebuild in the build directory
4. Start the new binary from the build directory
5. Verify the new version is running by checking modification time:
   ```bash
   stat ~/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama
   ps aux | grep hailo-ollama
   ```
