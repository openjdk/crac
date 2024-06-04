#!/bin/sh

# Hand-crafted launcher for portable CRaC tests. To be removed as soon as the
# tests are integrated with JTReg.
#
# Parameters:
# - BUILD_DIR -- path to the build directory where a JDK image has been built

BUILD_DIR=$1
if [ ! -d "$BUILD_DIR" ]; then
  echo "Usage: $0 BUILD_DIR"
  exit 1
fi

JAVA_HOME="$BUILD_DIR/images/jdk"
JAVA_HOME_EXP="$BUILD_DIR/jdk"
if [ ! -x "$JAVA_HOME/bin/java" ] | [ ! -x "$JAVA_HOME/bin/javac" ]; then
  echo "Cannot find JDK in $JAVA_HOME"
  exit 1
fi
if [ ! -x "$JAVA_HOME_EXP" ]; then
  echo "Cannot find exploded JDK in $JAVA_HOME_EXP"
  exit 1
fi

for JAVA_FILE in *.java; do
  PROG=${JAVA_FILE%.java}
  printf "\n\n##### %s\n" "$PROG"

  "$JAVA_HOME/bin/javac" "$JAVA_FILE"

  printf "\n%s: normal + compiled\n" "$PROG"
  rm -rf cr
  "$JAVA_HOME/bin/java" -XX:CREngine= -XX:CRaCCheckpointTo=cr "$PROG"
  printf "\n"
  "$JAVA_HOME/bin/java" -XX:CREngine= -XX:CRaCRestoreFrom=cr

  printf "\n%s: normal + launched\n" "$PROG"
  rm -rf cr
  "$JAVA_HOME/bin/java" -XX:CREngine= -XX:CRaCCheckpointTo=cr "$JAVA_FILE"
  printf "\n"
  "$JAVA_HOME/bin/java" -XX:CREngine= -XX:CRaCRestoreFrom=cr

  printf "\n%s: exploded + compiled\n" "$PROG"
  rm -rf cr
  "$JAVA_HOME_EXP/bin/java" -XX:CREngine= -XX:CRaCCheckpointTo=cr "$PROG"
  printf "\n"
  "$JAVA_HOME_EXP/bin/java" -XX:CREngine= -XX:CRaCRestoreFrom=cr

  printf "\n%s: exploded + launched\n" "$PROG"
  rm -rf cr
  "$JAVA_HOME_EXP/bin/java" -XX:CREngine= -XX:CRaCCheckpointTo=cr "$JAVA_FILE"
  printf "\n"
  "$JAVA_HOME_EXP/bin/java" -XX:CREngine= -XX:CRaCRestoreFrom=cr
done

rm -rf cr
