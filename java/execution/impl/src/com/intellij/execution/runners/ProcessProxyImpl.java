/*
 * Copyright 2000-2016 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.intellij.execution.runners;

import com.intellij.execution.process.ProcessHandler;
import com.intellij.openapi.util.Key;

import java.io.BufferedWriter;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * @author ven
 */
class ProcessProxyImpl implements ProcessProxy {
  public static final Key<ProcessProxyImpl> KEY = Key.create("ProcessProxyImpl");

  public static final String PROPERTY_BIN_PATH = "idea.launcher.bin.path";
  public static final String PROPERTY_PORT_NUMBER = "idea.launcher.port";
  public static final String LAUNCH_MAIN_CLASS = "com.intellij.rt.execution.application.AppMain";

  private static final int SOCKET_NUMBER_START = 7532;
  private static final int SOCKET_NUMBER = 100;
  private static final boolean[] ourUsedSockets = new boolean[SOCKET_NUMBER];

  private final int myPortNumber;
  private PrintWriter myWriter;
  private Socket mySocket;

  public static class NoMoreSocketsException extends Exception { }

  public ProcessProxyImpl() throws NoMoreSocketsException {
    myPortNumber = findFreePort();
    if (myPortNumber == -1) throw new NoMoreSocketsException();
  }

  private static int findFreePort() {
    synchronized (ourUsedSockets) {
      for (int j = 0; j < SOCKET_NUMBER; j++) {
        if (ourUsedSockets[j]) continue;
        try {
          ServerSocket s = new ServerSocket(j + SOCKET_NUMBER_START);
          s.close();
          ourUsedSockets[j] = true;
          return j + SOCKET_NUMBER_START;
        }
        catch (IOException ignore) { }
      }
    }
    return -1;
  }

  @Override
  public int getPortNumber() {
    return myPortNumber;
  }

  @Override
  @SuppressWarnings("FinalizeDeclaration")
  protected synchronized void finalize() throws Throwable {
    if (myWriter != null) {
      myWriter.close();
    }
    ourUsedSockets[myPortNumber - SOCKET_NUMBER_START] = false;
    super.finalize();
  }

  @Override
  public void attach(final ProcessHandler processHandler) {
    processHandler.putUserData(KEY, this);
  }

  @SuppressWarnings({"SocketOpenedButNotSafelyClosed", "IOResourceOpenedButNotSafelyClosed"})
  private synchronized void writeLine(String s) {
    if (myWriter == null) {
      try {
        if (mySocket == null) {
          mySocket = new Socket(InetAddress.getLoopbackAddress(), myPortNumber);
        }
        myWriter = new PrintWriter(new BufferedWriter(new OutputStreamWriter(mySocket.getOutputStream())));
      }
      catch (IOException e) {
        return;
      }
    }
    myWriter.println(s);
    myWriter.flush();
  }

  @Override
  public void sendBreak() {
    writeLine("BREAK");
  }

  @Override
  public void sendStop() {
    writeLine("STOP");
  }
}