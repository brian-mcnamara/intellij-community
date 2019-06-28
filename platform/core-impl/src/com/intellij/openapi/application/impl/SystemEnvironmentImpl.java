// Copyright 2000-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.
package com.intellij.openapi.application.impl;

import com.google.common.collect.ImmutableMap;
import com.intellij.openapi.components.ServiceManager;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.project.SystemEnvironment;
import org.jetbrains.annotations.NonNls;
import org.jetbrains.annotations.NotNull;

import java.util.Map;

/**
 * Created by brian.mcnamara on Jun 27 2019
 **/
public class SystemEnvironmentImpl implements SystemEnvironment {

  private Map<String, String> environment = System.getenv();

  public static SystemEnvironmentImpl getInstance(@NotNull final Project project) {
    return ServiceManager.getService(project, SystemEnvironmentImpl.class);
  }

  void setEnvironment(Map<String, String> environment) {
    this.environment = ImmutableMap.copyOf(environment);
  }
  @Override
  public Map<String, String> getEnv() {
    return environment;
  }
}
