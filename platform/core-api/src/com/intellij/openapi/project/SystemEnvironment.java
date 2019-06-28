// Copyright 2000-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.
package com.intellij.openapi.project;

import java.util.Map;

/**
 * Created by brian.mcnamara on Jun 27 2019
 **/
public interface SystemEnvironment {

  Map<String, String> getEnv();

  default String getEnv(String env) {
    return getEnv().get(env);
  }
}
