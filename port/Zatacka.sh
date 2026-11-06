#!/bin/bash
# Achtung, die Kurve! (ZATACKA.EXE, c. 1995): native port för BananaOS, båda panelerna.
# Ports-post: run-port kör den här med bash. Programmet ligger i /data/zatacka (deploy.sh lägger det där).
exec /data/zatacka/run-zatacka.sh "$@"
