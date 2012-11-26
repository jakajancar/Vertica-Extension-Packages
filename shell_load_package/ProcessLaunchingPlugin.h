/* Copyright (c) 2005 - 2012 Vertica, an HP company -*- C++ -*- */
#ifndef PROCESSLAUNCHINGPLUGIN_H
#define PROCESSLAUNCHINGPLUGIN_H

#include "Vertica.h"
#include "popen3.h"
#include <time.h>

using namespace Vertica;

//
// Superclass/mixin for User-Defined Load and Filter libraries which launch an
// external process.
//
class ProcessLaunchingPlugin {
protected:
    ProcessLaunchingPlugin(std::string cmd, std::vector<std::string> env);
    void setupProcess();
    StreamState pump(DataBuffer &input, InputState input_state, DataBuffer &output);
    void destroyProcess();
    ~ProcessLaunchingPlugin();

private:    
    std::string cmd;
    std::vector<std::string> env;
    Popen3Proc child;
    
    DataBuffer error;
    time_t first_error_time;
    
    void checkProcessStatus(pid_t pid);
};

#endif
