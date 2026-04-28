This folder contains a project that runs the electronics for the installation.

There are two targets, a Central and a Perpheral, each of which will be installed on an ESP32 microcontroller. One Central device will communicate with many Peripheral devices over I2C. Each Peripheral device will be set up with an ID, and then a control schema that describes what is connected to it - what devices, what pins, how it should be controlled etc. 

There should also be a GUI program (prefereably HTML/JS) that communicates with  Master or Peripheral devices over USB Serial or similar, that can: 
- edit the config of a Peripheral device directly connected (setting pins, control schemas etc)
- directly control the operation of a connected Peripheral device - setting speed/position/direction of actuators directly, running particular test programs etc.
- show a connected network of devices (initially just listing them, eventually showing a graphic layout) and allow a device to be selected and controlled
- send commands (e.g. stop everything) to the whole network

As much as possible, there should be centralised definitions of protocols and elements. Currently, this is started in the `shared` folder, as:
- control_schema.json: this gives an overview of the Elements and the way that they can be configured. If new Elements are added, they should appear here, and then as code in the Peripheral codebase (as well as other places like GUIs)
- example_elements.json: this is an example configuration, that could be loaded on a Perpheral board, specifying the Elements to load and the connections to make
- i2c_messages.json: a specification of the I2C messages to be sent and received

# Control Schema
Each peripheral device will control several actuators. These can be of different types, and combinations with particular control methods. Each Element in the control schema has:
- an ID, that can be used over I2C to identify that kind of element
- a list of config elements - it's not clear yet how they will be set, might be too complex for I2C, so only working with direct serial connection/JSON editing
- a list of parameters - these will be sent in realtime, as floating point numbers, over I2C (or serial etc.) and passed to the control algorithm to work with
Each Peripheral device should be able to be configured through the GUI program or directly by writing a JSON file that describes the connected elements using this schema

There is a currently a minimal example in control_schema.json - this should be gradually expanded, mostly by hand, to cover new kinds of controller and setup. The `i2c_messages.json` file is the start of a definition of all the i2c messages that should be sent and received on the bus. It's not entirely formal yet - there are implicit connections between e.g. the length values and the number of bytes to read; this should generally be handled in the code.

# Peripheral
The Peripheral device is probably written in C++ with Platformio. It should read a config.json file stored on the device - see the example in `example_elements.json`, and use this to set up a collection of Elements, each of which controls a bit of hardware. There should be a simple class hierarchy to make sure that each Element has:
- a method that takes a list of parameter values as floats and applies them to the current setup
- an update(time) method that is called periodically to update the functioning
- an all_stop() method that makes sure the physical elements are safe and still.
- a demo state that can be used to check that all functions of the element are working correctly
- other things that become necessary over time

This means that the Peripheral code should:
- have all the necessary libraries for all the elements that might be used
- read the JSON file at startup, and initialise objects for each element
- run a fairly tight update() loop that updates all of the Elements
- send out the loaded configuration over I2C when ready or when asked
- listen for I2C parameter updates, and pass the relevant values to the Elements
- listen for important I2C messages
- if directly connected to USB Serial:
    - listen for the messages described in i2c_messages.json in a simple textual form suitable for typing by hand, e.g. `stop` or `s` for stop, `p 1 0.4 0.6` to set the two parameters of element 1 to 0.4 and 0.6 respectively
    - recieve a new JSON Element configuration and store it

# Central
The Central device might be in CircuitPython or C++, either is fine. It should:
- Query devices on the I2C bus to find out what is connected and what their setup is
- Function as a USB MIDI device, translating control change messages to parameter updates
- Allow a connection over USB Serial for a GUI program to do control of all the connected elements(with float precision). This means sending a (probably JSON) representation of all the connected devices
- Allow for useful messages (e.g. `stop`) to be sent to all devices.
- It should not have particularly strong timing constraints - most realtime behaviour will be handled by the Peripheral devices.


# Future Plans
- At some point, this may become more distributed, so e.g. swapping I2C control for WiFi or Matter over Thread. Nothing to do about this now, other than keep the idea open that the Peripheral device should have multiple inputs