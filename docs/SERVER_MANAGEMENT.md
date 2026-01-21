# Server Management Script

## Overview

The `hailo-server.sh` script provides a simple interface for managing the Hailo-Ollama server.

## Location

- **On server**: `~/hailo-server.sh`
- **In repo**: `hailo-server.sh` (in project root)

## Usage

```bash
~/hailo-server.sh {start|stop|restart|status}
```

## Commands

### Start Server

```bash
~/hailo-server.sh start
```

Starts the Hailo-Ollama server. If the server is already running, it will restart it automatically.

**Output:**
```
Starting hailo-ollama server...
Binary: /home/stranded/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama
Log: /tmp/hailo-ollama.log
Server started successfully (PID: 7236)
Checking port 8000...
✓ Server is listening on port 8000
```

### Stop Server

```bash
~/hailo-server.sh stop
```

Stops the running server.

**Output:**
```
Stopping hailo-ollama server...
Server stopped successfully
```

### Restart Server

```bash
~/hailo-server.sh restart
```

Stops and then starts the server. This is the recommended way to deploy updates.

**Output:**
```
Stopping hailo-ollama server...
Server stopped successfully
Starting hailo-ollama server...
Binary: /home/stranded/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama
Log: /tmp/hailo-ollama.log
Server started successfully (PID: 7236)
Checking port 8000...
✓ Server is listening on port 8000
```

### Check Status

```bash
~/hailo-server.sh status
```

Shows the current server status, including:
- Process information (PID, command)
- Port status
- Binary location
- Recent log entries

**Output (when running):**
```
Server is running:
stranded    7236  0.0  0.0  54288 11408 ?        Sl   16:57   0:00 ./hailo-ollama

Port status:
LISTEN 0      4096         0.0.0.0:8000       0.0.0.0:*    users:(("hailo-ollama",pid=7236,fd=6))

Binary location:
/home/stranded/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama

Recent logs:
[32m I [0m|2026-01-21 16:57:46 1768978666147200| MyApp:Server running on port 8000
```

**Output (when stopped):**
```
Server is not running

Port status:
Port 8000 is free
```

## Configuration

The script uses the following defaults:

- **Project directory**: `~/hailo_model_zoo_genai`
- **Binary path**: `~/hailo_model_zoo_genai/build/src/apps/server/hailo-ollama`
- **Log file**: `/tmp/hailo-ollama.log`
- **PID file**: `/tmp/hailo-ollama.pid`

## Deployment Workflow

When deploying code updates:

```bash
# 1. SSH to server
ssh stranded

# 2. Update code (if needed)
cd ~/hailo_model_zoo_genai
git pull

# 3. Rebuild
cd build
cmake --build .

# 4. Restart server
~/hailo-server.sh restart

# 5. Verify it's running
~/hailo-server.sh status
```

## Viewing Logs

To watch logs in real-time:

```bash
tail -f /tmp/hailo-ollama.log
```

To view recent logs:

```bash
tail -50 /tmp/hailo-ollama.log
```

## Troubleshooting

### Server Won't Start

Check if port 8000 is already in use:
```bash
ss -tlnp | grep 8000
```

If something else is using the port, kill it:
```bash
~/hailo-server.sh stop
```

### Server Crashes

Check the logs for errors:
```bash
tail -100 /tmp/hailo-ollama.log
```

### Binary Not Found

If you see "Error: Server binary not found", rebuild the server:
```bash
cd ~/hailo_model_zoo_genai/build
cmake --build .
```

## Notes

- The script uses `pkill -9` for reliable process termination
- Starting an already-running server will automatically restart it
- The script excludes `tail` processes when checking server status
- Logs are appended to `/tmp/hailo-ollama.log` (not rotated)
