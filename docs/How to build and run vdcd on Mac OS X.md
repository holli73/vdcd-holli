# How to build and run the vdcd in XCode on OS X
As an Apple user, I'm using XCode as my primary development tool. Not only ObjC, but most of my C/C++ software starts there under Clang/LLVM, and only afterwards gets ported to the various embedded targets (usually Linux with older gcc).

Here's what you need to do if you want to build vdcd in XCode on OS X

## Prepare

### install XCode 6.x
Just from Mac App Store or see [https://developer.apple.com/xcode/](https://developer.apple.com/xcode/)
### install (home)brew
Go to [http://brew.sh](http://brew.sh) and follow instructions

### install needed packages from brew

	brew install json-c
	brew install protobuf-c
	brew install boost

### create a project directory

	mkdir <project dir of your choice>
	cd <project dir of your choice>

### clone the git repository

	git clone https://github.com/plan44/vdcd.git


## Build

- open the vdcd.xcodeproj and build it (cmd-B)

- configure the arguments in XCode (Product->Scheme->Edit Scheme..., under "Run" -> "Arguments". If you don't know what arguments you need, just try --help to get a summary.

A possible vdcd command line could be:

	vdcd --cfgapiport 8090 -l 7 --consoleio testbutton:button --consoleio testlamp:dimmer

## Run

Now you have a vdcd running, which can accept JSON queries on port 8090. Note that this is not a http server, you need to open socket connections to send JSON requests (LF delimited).

Check out the small api.php script in the *json\_api\_forwarder* folder if you want a http frontend for the API.

Assuming you have api.php installed at *http://localhost/api.php*, you can do the following from a browser:

### list the entire property tree
	http://localhost/api.php/vdc?method=getProperty&dSUID=root&name=%20

### list of all devices of a specific devices class

The static device class containes the devices statically created at vdcd startup through options on the command line (or via Web UI).

	http://localhost/api.php/vdc?method=getProperty&dsuid=root&x-p44-itemSpec=vdc:Static_Device_Container&name=%20
	
### list of all properties of a device by dSUID
	http://localhost/api.php/vdc?method=getProperty&name=%20&dSUID=9ADC13F7D59E5B0280EC4E22E273FA0600


## vdc API

See [www.digitalstrom.org/allianz/entwickler/architekturdokumente](https://www.digitalstrom.org/allianz/entwickler/architekturdokumente/) for the detailed vDC API specs.

Note that while the real vDC API is a protobuf API, the vdcd also exposes the same API as a JSON. The examples above use a simplified GET variant, for using the same functionality as described in the specs you need to use POST (or direct TCP socket connection) to send a JSON object containing either *method=\<methodname\>* or *notification=\<notificationname\>*, plus any parameter as described in the specs.
