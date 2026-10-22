# Guide d'entretien : expliquer ce projet

Avant de mettre ce projet sur ton CV, tu dois pouvoir répondre à chaque question ci-dessous sans regarder le code. Pour t'y préparer :

1. Lis `sql/002_functions.sql`. C'est le cœur du système, en 200 lignes.
2. Lis `freeswitch/scripts/route_handler.lua`.
3. Lance `make test` puis `make test-db`.
4. Fais un essai à blanc : `./router module2 test 15550001111 12125550000`.

## Le pitch (30 secondes)

« J'ai repris un routeur d'appels VoIP basé sur FreeSWITCH. Il fait passer chaque appel par un serveur intermédiaire, sur un numéro temporaire (un DID), puis restaure l'appelant et le numéro d'origine avant de livrer l'appel à l'opérateur final. J'en ai fait un audit et j'ai trouvé une trentaine de défauts : injections SQL depuis les en-têtes SIP, un crash avec un seul paquet UDP, deux interblocages, et un même DID donné à deux appels sous charge. J'ai déplacé la logique critique dans des fonctions PostgreSQL atomiques, avec `FOR UPDATE SKIP LOCKED` et des index partiels : le débit d'allocation est passé de 213 à 2 293 appels par seconde, sans aucune collision. Cinq suites de tests, dont un test SIP de bout en bout sous AddressSanitizer, tournent dans GitHub Actions à chaque push. »

## Questions probables

**Que fait le routeur, concrètement ?**
Un appel arrive de S1 avec l'appelant ANI-1 et le numéro DNIS-1. S2 prend un DID libre et envoie l'appel à S3 avec l'appelant DNIS-1 et le numéro DID. S3 rappelle S2 sur ce DID. S2 retrouve ANI-1 et DNIS-1 grâce au DID, le libère, et envoie l'appel à S4 avec les valeurs d'origine. Le DID est le seul lien entre l'aller et le retour.

**Pourquoi le DID est-il le point sensible ?**
Si deux appels reçoivent le même DID, le retour de S3 est livré au mauvais destinataire. Si un DID reste bloqué, la réserve s'épuise. Et si on accepte un retour sur un DID non attribué, n'importe qui peut injecter des appels vers S4 aux frais de l'entreprise.

**Comment avez-vous empêché deux appels d'obtenir le même DID ?**
L'ancien code faisait un `SELECT` puis un `UPDATE` séparé. Entre les deux, un autre appel pouvait lire la même ligne : 16 collisions sur 400 appels dans mon banc d'essai. Maintenant, tout se passe dans une seule fonction SQL, en une transaction. `FOR UPDATE` verrouille la ligne choisie, et `SKIP LOCKED` fait passer les autres transactions à la ligne suivante au lieu d'attendre. Le test `tests/sql/test_concurrency.sh` lance 40 appels simultanés sur 10 DID et vérifie qu'on obtient 10 DID tous différents.

**Pourquoi c'est 10 fois plus rapide ?**
`ORDER BY RANDOM()` trie tous les DID libres à chaque appel, donc le coût grandit avec la réserve. J'ai créé des index partiels qui ne contiennent que les DID libres, déjà dans l'ordre de distribution. PostgreSQL lit une seule entrée d'index. Un piège : avec `provider_id IS NULL`, la première colonne de l'index n'est pas fixée comme avec `=`, et PostgreSQL trie quand même. Je l'ai vu dans `EXPLAIN ANALYZE`, et c'est pour ça qu'il y a deux index.

**Pourquoi l'ordre « le moins récemment libéré » ?**
Pour ne pas redonner tout de suite un DID qu'on vient de libérer : un retour tardif ou retransmis de l'appel précédent pourrait encore arriver, et il doit être rejeté, pas livré au nouvel appel.

**Comment avez-vous trouvé le bug des en-têtes SIP ?**
La fonction d'extraction renvoyait un tampon `static` unique. Lire From après Call-ID écrasait donc le Call-ID. Je l'ai reproduit sur le binaire d'origine : la réponse SIP renvoyait l'en-tête To à la place du Call-ID, et la base enregistrait la même valeur dans trois colonnes. La correction utilise des tampons fournis par l'appelant. Le test unitaire `test_headers_do_not_alias` empêche une régression.

**Comment empêchez-vous l'injection SQL, alors que l'API Lua de FreeSWITCH n'a pas de requêtes paramétrées ?**
En défense en profondeur :
1. le script Lua n'accepte que des chiffres (avec un `+` facultatif) ;
2. il double quand même les apostrophes ;
3. la fonction SQL revérifie le format avec `router_is_number`.

Côté C, toutes les requêtes construites à partir de données externes passent par `PQexecParams`, où les valeurs voyagent séparément du texte SQL.

**Qu'est-ce que `break="never"` et pourquoi est-ce grave ?**
Dans un dialplan FreeSWITCH, plusieurs conditions dans une extension sont combinées par un ET logique, par défaut. `break="never"` dit de continuer même si la condition échoue. La vérification de l'adresse source ne servait donc à rien : n'importe quelle machine pouvait entrer dans le flux.

**Comment avez-vous testé sans FreeSWITCH ?**
À trois niveaux :
- le vrai script Lua tourne avec de faux objets `session` et `freeswitch`, mais une vraie base PostgreSQL ;
- le démon C tourne en mode redirection SIP, et un script Python joue S1, S3 et S4 avec des sockets UDP sur 127.0.0.1, 127.0.0.3 et 127.0.0.4 ;
- les fichiers XML générés sont validés par `xmllint`, puis on vérifie leur contenu.

**Pourquoi AddressSanitizer dans les tests ?**
Parce qu'un use-after-free ne plante pas toujours : il corrompt silencieusement la mémoire. ASan le transforme en erreur immédiate, avec la ligne fautive. Le test de bout en bout recharge les fournisseurs pendant que des appels arrivent, exactement le scénario du use-after-free d'origine.

**Quelles limites restent ?**
- En mode redirection (302), on ne peut pas réécrire l'appelant : il faut le mode FreeSWITCH.
- Le démon C utilise une seule connexion PostgreSQL protégée par un mutex. Un pool serait l'étape suivante.
- Le sous-système `validation` hérité dépend d'une table que rien n'alimente encore.

Je préfère les écrire dans le README que les cacher.

## Exercices pour te l'approprier

1. Ajoute une route `212[5-7][0-9]{8}` avec `route add`. Vérifie le fichier `route_N.xml` généré, puis `route test 212612345678`.
2. Dans `sql/002_functions.sql`, remets le `SELECT … ORDER BY RANDOM()` sans verrou et relance `make bench` : observe les collisions.
3. Écris un test Lua pour un DID appartenant au fournisseur S3 : il doit être choisi avant la réserve partagée.
