// Copyright 2000-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.
package org.jetbrains.idea.maven.server.security;

import java.security.Permission;

/**
 * Created by brian.mcnamara on Oct 22 2019
 **/
public class MavenRestrictedSecurityManager extends SecurityManager {

  @Override
  public void checkPermission(Permission perm) {
    //Prevent maven plugins from overriding our lax security manager
    if (perm.getName().equals("setSecurityManager")){
      throw new SecurityException("Illegal attempt to override security manager");
    }
  }
}
