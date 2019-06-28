// Copyright 2000-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.
package com.intellij.idea;

import java.util.List;
import java.util.Map;

/**
 * Created by brian.mcnamara on Jun 27 2019
 **/
public class ExternalStartupParams {
  private final String token;
  private final List<String> args;
  private final String path;
  private final Map<String, String> env;

  ExternalStartupParams(String token, List<String> args, String path, Map<String, String> env) {
    this.token = token;
    this.args = args;
    this.path = path;
    this.env = env;
  }

  public List<String> getArgs() {
    return args;
  }

  public String getPath() {
    return path;
  }

  public Map<String, String> getEnv() {
    return env;
  }

  String getToken() {
    return token;
  }
}
