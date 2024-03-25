#include "LibSerial.h"
#include <sstream>
#include <emscripten.h>
#include <emscripten/val.h>
#include <map>
#include <iostream>
#include <emscripten/bind.h>
#include <iomanip>

using namespace emscripten;

std::vector<int8_t> readBuffer;

extern "C" {
EMSCRIPTEN_KEEPALIVE
void dataCallback(int8_t *array, int length) {
    std::vector<int8_t> data(array, array + length);
    readBuffer.insert(readBuffer.end(), data.begin(), data.end());
}
}

namespace {


    EM_ASYNC_JS(void, clear_read_buffer, (int timeoutMs), {
        window.avrdudeLog = [...window.avrdudeLog, "Clearing read buffer"];
        const timeoutPromise = new Promise((resolve, _) => {
                setTimeout(() => {
                        resolve("Timeout");
                }, 100);
        });
        const timeoutPromiseRead = new Promise((resolve, _) => {
                setTimeout(() => {
                        resolve("Timeout");
                }, timeoutMs);
        });

        let i = 1;
        const readStream = window.activePort.readable.getReader();
        while (true) {
            window.avrdudeLog = [...window.avrdudeLog, "Attempt #" + i];
            const promise = new Promise(async (resolve, _) => {
                    while (true) {
                        const result = await Promise.race([readStream.read(), timeoutPromise]);
                        if (result === "Timeout")
                            break;
                    }
                    resolve("K");
            });
            const result = await Promise.race([promise, timeoutPromiseRead]);

            if (result !== "Timeout")
                break;
            if (i > 10)
                throw new Error('Timeout');
            i++;
        }
        readStream.releaseLock();
        window.avrdudeLog = [...window.avrdudeLog, "Read buffer cleared"];
    });

    EM_ASYNC_JS(void, read_data, (int timeoutMs), {
        const reader = window.activePort.readable.getReader();
        async function receive() {
            // try to read data with the timeoutMs / 10 as the timeout
            const smallerTimeout = new Promise((resolve, _) => {
                setTimeout(() => {
                    resolve("Timeout");
                }, timeoutMs / 10);
            });

            for (let i = 0; i < 5; i++) {
                const { value, done } = await Promise.race([reader.read(), smallerTimeout]);
                if (done) {
                    await new Promise(resolve => setTimeout(resolve, timeoutMs / 10));
                }
                if (value) {
                    return value;
                }
            }
            return new Uint8Array();
        }

        async function timeout(timeoutMs) {
            await new Promise(resolve => setTimeout(resolve, timeoutMs));
            return "timeout";
        }

        while (true) {
            let result = await Promise.race([receive(), timeout(timeoutMs)]);

            if (result instanceof Uint8Array && result.length > 0) {
                //console.log("Received: ", result);
                // convert data into an readable string formated like this 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
                const printResult = Array.from(result).join(",");
                window["avrdudeLog"] = [...window["avrdudeLog"], "Received: " + printResult];
                const ptr = window.funcs._malloc(result.length * Uint8Array.BYTES_PER_ELEMENT);
                window.funcs.HEAPU8.set(result, ptr);

                // Call the C++ function with the pointer and the length of the array
                window.funcs._dataCallback(ptr, result.length);
                break;
            } else {
                window["avrdudeLog"] = [...window["avrdudeLog"], "Timeout"];
                break;
            }
        }
        reader.releaseLock();
        return;
    });

    // clang-format off
    // get serial options as an EM_VAL
    EM_ASYNC_JS(EM_VAL, open_serial_port, (EM_VAL cppSerialOpts), {
        let serialOpts = Emval.toValue(cppSerialOpts);
        serialOpts.bufferSize = 1024*2;
        let port;
        if (!window.activePort) {
            port = await navigator.serial.requestPort();
            await port.open(serialOpts);
        } else {
            port = window.activePort;
            // check if open otherwise open port
            if (!port.readable || !port.writable) {
                await port.open(serialOpts);
            }
        }
        // reset the arduino by setting the DTR signal at baud rate 1200
        await port.close();
        await port.open({baudRate: 1200});
        await port.setSignals({dataTerminalReady: false});
        await new Promise(resolve => setTimeout(resolve, 100));
        await port.setSignals({dataTerminalReady: true});
        await port.close();
        // open the port with the correct baud rate
        await port.open(serialOpts);
        // set up an activePort variable on the window, so it can be accessed from everywhere
        window.activePort = port;
        window.writeStream = port.writable.getWriter();
        window["avrdudeLog"] = [];
        return port;
    });

    EM_ASYNC_JS(void, set_serial_port_options, (EM_VAL port, EM_VAL serialOpts), {
        if (port.readable && port.writable) {
            await port.close();
        }
        await port.open(serialOpts);
    });

    EM_ASYNC_JS(void, close_serial_port, (EM_VAL port), {
        window.writeStream.releaseLock();
        await port.close();
    });

    EM_ASYNC_JS(bool, is_serial_port_open, (EM_VAL port), {
        return port.readable && port.writable;
    });

    EM_ASYNC_JS(void, set_dts_rts, (bool is_on), {
        await window.activePort.setSignals({dataTerminalReady: is_on, requestToSend: is_on});
    });


    EM_ASYNC_JS(void, write_data, (EM_VAL data), {
        data = new Uint8Array(Emval.toValue(data));
        //console.log("Sending: ", data);
        // convert data into an readable string formated like this 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        const printData = Array.from(data).join(",");
        window["avrdudeLog"] = [...window["avrdudeLog"], "Sending: " + printData];
        const port = window.activePort;
        await window.writeStream.ready;
        await window.writeStream.write(data);
        await window.writeStream.ready;
        // delay for 100ms to make sure the data is sent
        await new Promise(resolve => setTimeout(resolve, 250));
    });

    val generateSerialOptions(const std::map<std::string, int>& serialOptions) {
        val&& serialOpts = val::object();
        for (auto& [key, value] : serialOptions) {
            if (key == "flowControl" || key == "parity" || key == "stopBits") {
                printf("Not implemented yet\n");
                printf("Key: %s, Value: %d\n", key.c_str(), value);
                // Handle specific cases for flowControl, parity, and stopBits if needed
                // ...
            } else {
                serialOpts.set(key, value);
            }
        }
        return serialOpts;
    }

    void setSerialOptions(EM_VAL port ,const std::map<std::string, int>& serialOptions)
    {

        set_serial_port_options(port, generateSerialOptions(serialOptions).as_handle());
    }

}

int serialPortOpen(int baudRate) {
    val opts = generateSerialOptions({{"baudRate", baudRate}});
    if (opts.isUndefined() || opts.isNull()) {
        std::cerr << "Invalid options for serialPortOpen" << std::endl;
        return -1;
    }
    open_serial_port(opts.as_handle());
    return 0;
}

void setDtrRts(bool is_on) {
    set_dts_rts(is_on);
}

void serialPortDrain(int timeout) {
    // print the length of the timeout

    readBuffer.clear();
    clear_read_buffer(timeout);
}

void serialPortWrite(const unsigned char *buf, size_t len) {
    std::vector<unsigned char> data(buf, buf + len);
    write_data(val(typed_memory_view(data.size(), data.data())).as_handle());
}

int serialPortRecv(unsigned char *buf, size_t len, int timeoutMs) {
    std::vector<unsigned char> data = {};
    data.reserve(len);
    // check if there is leftover data from previous reads
    if (!readBuffer.empty()) {
        if (readBuffer.size() >= len) {
            data = std::vector<unsigned char>(readBuffer.begin(), readBuffer.begin() + len);
            readBuffer.erase(readBuffer.begin(), readBuffer.begin() + len);
            std::copy(data.begin(), data.end(), buf);
            return 0;
        } else {
            data = std::vector<unsigned char>(readBuffer.begin(), readBuffer.end());
            readBuffer.clear();
        }
    }
    read_data(timeoutMs);
    if (!readBuffer.empty()) {
        // check how much data is needed and add that much to the buffer
        if (readBuffer.size() >= len) {
            data = std::vector<unsigned char>(readBuffer.begin(), readBuffer.begin() + len);
            readBuffer.erase(readBuffer.begin(), readBuffer.begin() + len);
        } else {
            data = std::vector<unsigned char>(readBuffer.begin(), readBuffer.end());
            readBuffer.clear();
        }
    }
    if (data.empty()) {
        return -1;
    } else {
        std::copy(data.begin(), data.end(), buf);
    }
    return 0;
}