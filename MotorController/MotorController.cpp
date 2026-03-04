#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <conio.h>
#include <string>
#include <sstream>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

HANDLE hSerial;
std::atomic<long> currentEncoder{ 0 };
bool monitorMode = true;  // Start with display ON
bool debugMode = false;   // Show raw serial data
volatile bool running = true;

// UDP settings
SOCKET udpSocket;
sockaddr_in destAddr;
bool udpEnabled = false;

HANDLE readerThread;

bool initUDP(const char* ip, int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        return false;
    }

    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &destAddr.sin_addr);

    return true;
}

void sendUDP(long encoderValue) {
    if (!udpEnabled) return;
    char buffer[32];
    sprintf_s(buffer, sizeof(buffer), "%ld", encoderValue);
    sendto(udpSocket, buffer, (int)strlen(buffer), 0, (sockaddr*)&destAddr, sizeof(destAddr));
}

void cleanupUDP() {
    if (udpSocket != INVALID_SOCKET) {
        closesocket(udpSocket);
    }
    WSACleanup();
}

bool connectSerial(const char* portName) {
    hSerial = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cout << "Error: Cannot open " << portName << std::endl;
        return false;
    }

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);

    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;

    SetCommState(hSerial, &dcbSerialParams);

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    return true;
}

void sendChar(char cmd) {
    DWORD bytesWritten;
    WriteFile(hSerial, &cmd, 1, &bytesWritten, NULL);
}

void sendCommand(const std::string& cmd) {
    DWORD bytesWritten;
    WriteFile(hSerial, cmd.c_str(), (DWORD)cmd.length(), &bytesWritten, NULL);
}

// Background thread that reads serial
DWORD WINAPI serialReaderThread(LPVOID lpParam) {
    char buffer[256];
    std::string lineBuffer = "";
    DWORD bytesRead;

    while (running) {
        if (ReadFile(hSerial, buffer, 255, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';

            // Debug: show raw data
            if (debugMode) {
                printf("[RAW: %s]", buffer);
                fflush(stdout);
            }

            // Parse line by line
            for (DWORD i = 0; i < bytesRead; i++) {
                char c = buffer[i];

                if (c == '\n' || c == '\r') {
                    if (!lineBuffer.empty()) {
                        // Try to parse as number
                        bool isNumber = true;
                        for (size_t j = 0; j < lineBuffer.length(); j++) {
                            if (!isdigit(lineBuffer[j]) && lineBuffer[j] != '-') {
                                isNumber = false;
                                break;
                            }
                        }

                        if (isNumber && !lineBuffer.empty()) {
                            currentEncoder.store(atol(lineBuffer.c_str()));
                        }
                        lineBuffer.clear();
                    }
                }
                else {
                    lineBuffer += c;
                }
            }
        }
    }
    return 0;
}

void printHelp() {
    std::cout << "\n========== Motor Control ==========" << std::endl;
    std::cout << "C      = Forward (continuous)" << std::endl;
    std::cout << "X      = Backward (continuous)" << std::endl;
    std::cout << "S      = Stop" << std::endl;
    std::cout << "W      = Speed UP" << std::endl;
    std::cout << "A      = Speed DOWN" << std::endl;
    std::cout << "V      = Show current speed" << std::endl;
    std::cout << "R      = Rotate degrees (enter value)" << std::endl;
    std::cout << "E      = Show encoder count" << std::endl;
    std::cout << "Z      = Reset encoder to zero" << std::endl;
    std::cout << "M      = Toggle real-time display" << std::endl;
    std::cout << "D      = Toggle debug mode (show raw serial)" << std::endl;
    std::cout << "H      = Show this help" << std::endl;
    std::cout << "Q      = Quit" << std::endl;
    std::cout << "====================================" << std::endl;
}

int main() {
    if (initUDP("127.0.0.1", 5005)) {
        udpEnabled = true;
        std::cout << "UDP initialized - sending to 127.0.0.1:5005" << std::endl;
    }

    std::string port;
    std::cout << "Enter COM port (e.g., COM3): ";
    std::cin >> port;

    std::string fullPort = "\\\\.\\" + port;

    if (!connectSerial(fullPort.c_str())) {
        return 1;
    }

    std::cout << "Connected to " << port << " at 115200 baud" << std::endl;
    Sleep(500);

    printHelp();

    // Start reader thread with elevated priority
    readerThread = CreateThread(NULL, 0, serialReaderThread, NULL, 0, NULL);
    SetThreadPriority(readerThread, THREAD_PRIORITY_ABOVE_NORMAL);

    std::cout << "\nPress D to toggle debug mode and see raw serial data" << std::endl;
    std::cout << "Press M to toggle encoder display\n" << std::endl;

    int tick = 0;
    const char spinner[] = "|/-\\";
    long lastSentEncoder = 0;
    DWORD lastDisplayTime = GetTickCount();

    while (running) {
        if (_kbhit()) {
            char key = _getch();

            if (key == 'q' || key == 'Q') {
                sendChar('S');
                std::cout << "\nExiting..." << std::endl;
                running = false;
                break;
            }

            if (key == 'h' || key == 'H') {
                printHelp();
                continue;
            }

            if (key == 'd' || key == 'D') {
                debugMode = !debugMode;
                std::cout << "\n[DEBUG MODE: " << (debugMode ? "ON" : "OFF") << "]" << std::endl;
                continue;
            }

            if (key == 'm' || key == 'M') {
                monitorMode = !monitorMode;
                std::cout << "\n[DISPLAY: " << (monitorMode ? "ON" : "OFF") << "]" << std::endl;
                continue;
            }

            if (key == 'r' || key == 'R') {
                std::cout << "\nEnter degrees: ";
                float degrees;
                std::cin >> degrees;
                std::stringstream ss;
                ss << "R" << degrees;
                sendCommand(ss.str());
                std::cout << "Rotating..." << std::endl;
                continue;
            }

            if (key == 'z' || key == 'Z') {
                sendChar('Z');
                currentEncoder.store(0);
                std::cout << "\n[RESET]" << std::endl;
                continue;
            }

            if (key == 'e' || key == 'E') {
                std::cout << "\nEncoder: " << currentEncoder.load() << std::endl;
                continue;
            }

            // Motor commands
            if (key == 'c' || key == 'C') { sendChar('C'); std::cout << "\n>> FORWARD" << std::endl; }
            if (key == 'x' || key == 'X') { sendChar('X'); std::cout << "\n>> BACKWARD" << std::endl; }
            if (key == 's' || key == 'S') { sendChar('S'); std::cout << "\n>> STOPPED" << std::endl; }
            if (key == 'w' || key == 'W') { sendChar('W'); std::cout << "\n>> SPEED UP" << std::endl; }
            if (key == 'a' || key == 'A') { sendChar('A'); std::cout << "\n>> SPEED DOWN" << std::endl; }
            if (key == 'v' || key == 'V') { sendChar('V'); }
        }

        // Send to Unity only when value changes
        long enc = currentEncoder.load();
        if (enc != lastSentEncoder) {
            sendUDP(enc);
            lastSentEncoder = enc;
        }

        // Throttle display to ~every 50ms to avoid console overhead
        DWORD now = GetTickCount();
        if (monitorMode && !debugMode && (now - lastDisplayTime >= 50)) {
            printf("\r[%c] Encoder: %-10ld", spinner[tick % 4], enc);
            fflush(stdout);
            tick++;
            lastDisplayTime = now;
        }

        Sleep(0);  // Yield timeslice without fixed delay
    }

    WaitForSingleObject(readerThread, 1000);
    CloseHandle(readerThread);
    CloseHandle(hSerial);
    cleanupUDP();
    return 0;
}