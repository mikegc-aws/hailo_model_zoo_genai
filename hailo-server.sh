#!/bin/bash

# Hailo-Ollama Server Control Script
# Usage: ./hailo-server.sh {start|stop|restart|status}

PROJECT_DIR="$HOME/hailo_model_zoo_genai"
BINARY_PATH="$PROJECT_DIR/build/src/apps/server/hailo-ollama"
LOG_FILE="/tmp/hailo-ollama.log"
PID_FILE="/tmp/hailo-ollama.pid"

stop_server() {
    echo "Stopping hailo-ollama server..."
    pkill -9 hailo-ollama 2>/dev/null
    
    # Wait for process to die
    sleep 2
    
    # Verify it's stopped (exclude grep and tail processes)
    if ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep' > /dev/null; then
        echo "Warning: Server may still be running"
        ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep'
        return 1
    else
        echo "Server stopped successfully"
        rm -f "$PID_FILE"
        return 0
    fi
}

start_server() {
    # First check if server is already running (exclude tail processes)
    if ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep' > /dev/null; then
        echo "Server is already running. Restarting..."
        stop_server || exit 1
    fi
    
    # Verify binary exists
    if [ ! -f "$BINARY_PATH" ]; then
        echo "Error: Server binary not found at $BINARY_PATH"
        echo "Please build the server first:"
        echo "  cd $PROJECT_DIR/build"
        echo "  cmake --build ."
        exit 1
    fi
    
    echo "Starting hailo-ollama server..."
    echo "Binary: $BINARY_PATH"
    echo "Log: $LOG_FILE"
    
    # Change to the server directory and start
    cd "$PROJECT_DIR/build/src/apps/server" || exit 1
    nohup ./hailo-ollama > "$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    echo $SERVER_PID > "$PID_FILE"
    
    # Wait a moment for server to start
    sleep 3
    
    # Check if server is running (exclude tail processes)
    if ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep' > /dev/null; then
        echo "Server started successfully (PID: $SERVER_PID)"
        echo "Checking port 8000..."
        if ss -tlnp 2>/dev/null | grep -q 8000; then
            echo "✓ Server is listening on port 8000"
        else
            echo "⚠ Warning: Server running but not listening on port 8000 yet"
            echo "Check logs: tail -f $LOG_FILE"
        fi
        return 0
    else
        echo "Error: Server failed to start"
        echo "Check logs: tail $LOG_FILE"
        return 1
    fi
}

status_server() {
    if ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep' > /dev/null; then
        echo "Server is running:"
        ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep'
        echo ""
        echo "Port status:"
        ss -tlnp 2>/dev/null | grep 8000 || echo "Not listening on port 8000"
        echo ""
        echo "Binary location:"
        ps aux | grep -E '[h]ailo-ollama' | grep -v 'tail' | grep -v 'grep' | awk '{print $2}' | head -1 | xargs -I {} readlink -f /proc/{}/exe 2>/dev/null || echo "Unknown"
        echo ""
        echo "Recent logs:"
        tail -10 "$LOG_FILE"
    else
        echo "Server is not running"
        echo ""
        echo "Port status:"
        ss -tlnp 2>/dev/null | grep 8000 || echo "Port 8000 is free"
    fi
}

case "$1" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        sleep 1
        start_server
        ;;
    status)
        status_server
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        echo ""
        echo "Commands:"
        echo "  start   - Start the server (restarts if already running)"
        echo "  stop    - Stop the server"
        echo "  restart - Stop and start the server"
        echo "  status  - Show server status and logs"
        exit 1
        ;;
esac

exit $?
