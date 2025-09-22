# RRTester Application

The RRTester application demonstrates the behavior of five threads within a **feedback scheduling** context. Through messages displayed on the screen, the application shows each thread's name, current scheduling queue, remaining service time, and current state.

## Modules

In RIOT, applications are built using **modules**, which are self-contained units of code that provide specific functionality to the application. Technically, a module is a folder containing source code, a Makefile, and optionally API definitions in one or more header files.

Every module must have a Makefile in RIOT's metadata. If the module name differs from the folder name, the `MODULE` macro must be set explicitly. In the case of RRTester, the Makefile defines only the module responsible for initializing the threads and explicitly includes the timer module.

## How It Works

The application was not built entirely from scratch; it is based on the "\_thread_duel_" example.  

At startup, the application generates the threads and immediately passes them to the scheduler. The scheduler manages thread execution according to the specified rules. Additionally, the code contains functions created specifically to display messages for aesthetic purposes.

For this project, the threads alternate with a **0.5s period**, and their service times are:

1. `T_A = 3s`  
2. `T_B = 6s`  
3. `T_C = 4s`  
4. `T_D = 5s`  
5. `T_E = 2s`  

