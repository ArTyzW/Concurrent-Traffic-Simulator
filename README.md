# Concurrent Traffic Simulator

A POSIX-compliant multi-process traffic intersection simulation written in C. The project models real-time vehicle movement and right-of-way logic across a 4-way intersection using Linux Inter-Process Communication (IPC) primitives.

---

## Architecture & Project Structure

- **`incrocio.c`**: Main coordinator process. Manages green lights, tracks vehicle states in shared memory, coordinates crossing turns based on traffic priority rules, and logs results.
- **`garage.c`**: Vehicle spawner process. Spawns individual car processes in structured batches and communicates with the coordinator.
- **`incrocio.h`**: Core logic header containing priority routines (`GetNextCar`, `EstraiDirezione`) that determine right-of-way according to highway code rules.
- **`run.sh`**: Automation script for compilation, execution, graceful termination via signals, and log verification.

---

## Technical Features

- **Process Synchronization**: Utilizes POSIX named semaphores (`sem_open`, `sem_wait`, `sem_post`) to sequence vehicle crossings without busy-waiting/polling.
- **Shared Memory**: Uses POSIX shared memory (`shm_open`, `mmap`) for high-performance inter-process state sharing between the garage and the intersection coordinator.
- **Signal Handling**: Handles `SIGTERM` for graceful teardown, releasing shared memory segments (`shm_unlink`) and unlinking named semaphores (`sem_unlink`).

---

## How to Run

1. Make the execution script executable:
   ```bash
   chmod +x run.sh
Run the simulation:

  '''bash
  
    ./run.sh

Press Enter in the terminal to stop the simulation. The script will send a termination signal and automatically compare incrocio.txt and auto.txt to verify log consistency.
