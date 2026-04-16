# Garbage Collection Visualizer

An interactive, visual demonstration of three core memory management/garbage collection strategies, powered by a C backend and an HTML5 Canvas frontend.

This educational tool provides real-time visualization of how different algorithms manage heap memory, detect cycles, and track object lifetimes. It also features a real-time monitor for process RAM usage and page faults.

## Features

- **Reference Counting**: Visualizes immediate deallocation when reference counts reach zero, and demonstrates how cyclic references cause permanent memory leaks.
- **Mark & Sweep**: Animates the tracing garbage collector step-by-step. Starting from root nodes, it marks all reachable objects (gray -> black) and safely reclaims unvisited (white) objects, smoothly resolving cycles.
- **Generational Collection**: Features a dual-zone (young/old generation) visualizer. Watch objects survive minor GCs, get promoted after surviving multiple collections, and get collected during major GCs.
- **Performance Simulation**: Adjust allocation rate and object lifetime sliders to see simulated performance metrics (latency, throughput, memory overhead) for each strategy based on real-world workload characteristics.
- **OS-Level Analytics**: Streams real process metrics (RAM and Page Faults) from the C backend directly to the visualizer UI.

## Architecture

The project consists of two modules:

1. **C Backend (`gc_server.c`)**: Defines the algorithms natively, managing physical memory, mock heap topologies, and running an embedded HTTP server (Windows Sockets, `winsock2`) on port `8080`.
2. **Web Frontend (`index.html` & `styles.css`)**: Built with HTML, vanilla CSS (modern aesthetics, glassmorphism), and vanilla JS (HTML5 Canvas rendering). It queries the C backend via REST API.

## Getting Started

### Prerequisites

- GCC (GNU Compiler Collection) or MinGW on Windows.
- A modern web browser.

### Running on Windows

You can build and start the server automatically using the provided batch script:

```bash
# 1. Build and start the backend
.\build_and_run.bat
```

Alternatively, compile it yourself using GCC:

```bash
gcc -Wall -O2 gc_server.c -o gc_server.exe -lws2_32 -lpsapi
.\gc_server.exe
```

### Viewing the UI

Once the backend is running (`Backend started on http://localhost:8080`), simply open the `index.html` file in your preferred web browser:

```bash
# Double-click 'index.html' or open it via terminal
start index.html
```

## How to interact

- Use the tabs to switch between algorithms.
- **Allocate** new objects and **connect/disconnect** them to see how the GC behaves.
- Use **GC Step** in Mark & Sweep to watch the algorithm traverse the graph.
- Create **Cycles** in Reference Counting to observe memory leaks.
- Keep an eye on the **Process RAM** and **Page Faults** at the top right header, which update in real time.

## License

MIT License
