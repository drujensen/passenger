// Included in DirectSpawnerTest.cpp and SmartSpawnerTest.cpp.

	#define SETUP_USER_SWITCHING_TEST() \
		if (geteuid() != 0) { \
			return; \
		} \
		TempDirCopy copy("stub/wsgi", "tmp.wsgi"); \
		addUserSwitchingCode(); \
		\
		DeleteFileEventually info1("/tmp/info.txt"); \
		DeleteFileEventually info2("/tmp/info2.txt"); \
		\
		SpawnerPtr spawner; \
		Options options; \
		options = createOptions(); \
		options.appRoot = "tmp.wsgi"; \
		options.appType = "wsgi"; \
		options.defaultUser = testConfig["default_user"].asCString(); \
		options.defaultGroup = testConfig["default_group"].asCString(); \
		spawner = createSpawner(options)

	#define RUN_USER_SWITCHING_TEST() \
		spawner->spawn(options); \
		BufferedIO io(FileDescriptor(open("/tmp/info.txt", O_RDONLY))); \
		uid_t uid = (uid_t) atol(io.readLine().c_str()); \
		gid_t gid = (gid_t) atol(io.readLine().c_str()); \
		string groups = strip(io.readLine()); \
		/* Avoid compiler warning. */ \
		(void) uid; (void) gid; (void) groups

	typedef shared_ptr<Spawner> SpawnerPtr;
	
	static void addUserSwitchingCode() {
		FILE *f = fopen("tmp.wsgi/passenger_wsgi.py", "a");
		fputs(
			"\n"
			"import os\n"
			"f = open('/tmp/info.txt', 'w')\n"
			"f.write(str(os.getuid()) + '\\n')\n"
			"f.write(str(os.getgid()) + '\\n')\n"
			"f.write(os.popen('groups').read() + '\\n')\n"
			"f.close()\n",
			f);
		fclose(f);

		rename("tmp.wsgi/passenger_wsgi.py", "tmp.wsgi/passenger_wsgi.py.real");
		symlink("passenger_wsgi.py.real", "tmp.wsgi/passenger_wsgi.py");
	}

	static void checkin(ProcessPtr process, Connection *conn) {
		process->sockets->front().checkinConnection(*conn);
	}

	static string userNameForUid(uid_t uid) {
		return getpwuid(uid)->pw_name;
	}

	static string groupNameForGid(gid_t gid) {
		return getgrgid(gid)->gr_name;
	}

	static uid_t uidFor(const string &userName) {
		return getpwnam(userName.c_str())->pw_uid;
	}

	static gid_t gidFor(const string &groupName) {
		return getgrnam(groupName.c_str())->gr_gid;
	}

	static string primaryGroupFor(const string &userName) {
		gid_t gid = getpwnam(userName.c_str())->pw_gid;
		return getgrgid(gid)->gr_name;
	}

	static string strip(const string &str) {
		if (!str.empty() && str[str.size() - 1] == '\n') {
			string result = str;
			result.erase(str.size() - 1,  1);
			return result;
		} else {
			return str;
		}
	}

	struct TemporarilyRedirectStderr {
		FileDescriptor oldStderr, newStderr;

		TemporarilyRedirectStderr(const string &filename) {
			oldStderr = FileDescriptor(dup(STDERR_FILENO));
			newStderr = FileDescriptor(open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
			if (oldStderr == -1 || newStderr == -1) {
				throw RuntimeException("Cannot dup or open file descriptor");
			}
			dup2(newStderr, STDERR_FILENO);
		}

		~TemporarilyRedirectStderr() {
			dup2(oldStderr, STDERR_FILENO);
		}
	};
	
	TEST_METHOD(1) {
		// Basic spawning test.
		Options options = createOptions();
		options.appRoot      = "stub/rack";
		options.startCommand = "ruby\1" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		ProcessPtr process = spawner->spawn(options);
		ensure_equals(process->sockets->size(), 1u);
		
		Connection conn = process->sockets->front().checkoutConnection();
		ScopeGuard guard(boost::bind(checkin, process, &conn));
		writeExact(conn.fd, "ping\n");
		ensure_equals(readAll(conn.fd), "pong\n");
	}

	TEST_METHOD(2) {
		// It enforces the given start timeout.
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "sleep\1" "60";
		options.startupFile  = ".";
		options.startTimeout = 300;
		SpawnerPtr spawner = createSpawner(options);
		try {
			spawner->spawn(options);
			fail("Timeout expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_TIMEOUT);
		}
	}

	TEST_METHOD(3) {
		// Any protocol errors during startup are caught and result
		// in exceptions.
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "echo\1" "!> hello world";
		options.startupFile  = ".";
		SpawnerPtr spawner = createSpawner(options);
		try {
			spawner->spawn(options);
			fail("Exception expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_PROTOCOL_ERROR);
		}
	}

	TEST_METHOD(4) {
		// The application may respond with a special Error response,
		// which will result in a SpawnException with the content.
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "perl\1" "start_error.pl";
		options.startupFile  = "start_error.pl";
		SpawnerPtr spawner = createSpawner(options);
		try {
			spawner->spawn(options);
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_EXPLAINABLE_ERROR);
			ensure_equals(e.getErrorPage(),
				"He's dead, Jim!\n"
				"Relax, I'm a doctor.\n");
		}
	}

	TEST_METHOD(5) {
		// The start timeout is enforced even while reading the error
		// response.
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "perl\1" "start_error.pl\1" "freeze";
		options.startupFile  = "start_error.pl";
		options.startTimeout = 300;
		SpawnerPtr spawner = createSpawner(options);
		try {
			spawner->spawn(options);
			fail("Timeout expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_TIMEOUT);
		}
	}

	TEST_METHOD(6) {
		// The reported PID is correct.
		Options options = createOptions();
		options.appRoot      = "stub/rack";
		options.startCommand = "ruby\1" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		ProcessPtr process = spawner->spawn(options);
		ensure_equals(process->sockets->size(), 1u);
		
		Connection conn = process->sockets->front().checkoutConnection();
		ScopeGuard guard(boost::bind(checkin, process, &conn));
		writeExact(conn.fd, "pid\n");
		ensure_equals(readAll(conn.fd), toString(process->pid) + "\n");
	}
	
	TEST_METHOD(7) {
		// Custom environment variables can be passed.
		Options options = createOptions();
		options.appRoot = "stub/rack";
		options.startCommand = "ruby\1" "start.rb";
		options.startupFile  = "start.rb";
		options.environmentVariables.push_back(make_pair("PASSENGER_FOO", "foo"));
		options.environmentVariables.push_back(make_pair("PASSENGER_BAR", "bar"));
		SpawnerPtr spawner = createSpawner(options);
		ProcessPtr process = spawner->spawn(options);
		ensure_equals(process->sockets->size(), 1u);
		
		Connection conn = process->sockets->front().checkoutConnection();
		ScopeGuard guard(boost::bind(checkin, process, &conn));
		writeExact(conn.fd, "envvars\n");
		string envvars = readAll(conn.fd);
		ensure("(1)", envvars.find("PASSENGER_FOO = foo\n") != string::npos);
		ensure("(2)", envvars.find("PASSENGER_BAR = bar\n") != string::npos);
	}

	TEST_METHOD(8) {
		// Any raised SpawnExceptions take note of the process's environment variables.
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "echo\1" "!> hello world";
		options.startupFile  = ".";
		options.environmentVariables.push_back(make_pair("PASSENGER_FOO", "foo"));
		SpawnerPtr spawner = createSpawner(options);
		try {
			spawner->spawn(options);
			fail("Exception expected");
		} catch (const SpawnException &e) {
			ensure(containsSubstring(e["envvars"], "PASSENGER_FOO=foo\n"));
		}
	}

	TEST_METHOD(9) {
		// It raises an exception if the user does not have a access to one
		// of the app root's parent directories, or the app root itself.
		runShellCommand("mkdir -p tmp.check/a/b/c");
		TempDirCopy dir("stub/rack", "tmp.check/a/b/c/d");
		TempDir dir2("tmp.check");

		char buffer[PATH_MAX];
		string cwd = getcwd(buffer, sizeof(buffer));

		Options options = createOptions();
		options.appRoot = "tmp.check/a/b/c/d";
		options.appType = "rack";
		SpawnerPtr spawner = createSpawner(options);

		if (getuid() != 0) {
			// TODO: implement this test for root too
			runShellCommand("chmod 000 tmp.check/a/b/c/d");
			runShellCommand("chmod 600 tmp.check/a/b/c");
			runShellCommand("chmod 600 tmp.check/a");

			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(1)", containsSubstring(e.getErrorPage(),
					"the parent directory '" + cwd + "/tmp.check/a' has wrong permissions"));
			}

			runShellCommand("chmod 700 tmp.check/a");
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(2)", containsSubstring(e.getErrorPage(),
					"the parent directory '" + cwd + "/tmp.check/a/b/c' has wrong permissions"));
			}

			runShellCommand("chmod 700 tmp.check/a/b/c");
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(3)", containsSubstring(e.getErrorPage(),
					"However this directory is not accessible because it has wrong permissions."));
			}

			runShellCommand("chmod 700 tmp.check/a/b/c/d");
			spawner->spawn(options); // Should not throw.
		}
	}

	TEST_METHOD(10) {
		// Test that the spawned process can still write to its stderr
		// after the corresponding Process has been destroyed.
		DeleteFileEventually d("tmp.output");
		Options options = createOptions();
		options.appRoot = "stub/rack";
		SpawnerPtr spawner = createSpawner(options);
		ProcessPtr process = spawner->spawn(options);
		
		SessionPtr session = process->newSession();
		session->initiate();
		
		const char header[] =
			"REQUEST_METHOD\0GET\0"
			"PATH_INFO\0/print_stderr\0";
		string data(header, sizeof(header) - 1);
		data.append("PASSENGER_CONNECT_PASSWORD");
		data.append(1, '\0');
		data.append(process->connectPassword);
		data.append(1, '\0');

		{
			TemporarilyRedirectStderr redirect("tmp.output");
			writeScalarMessage(session->fd(), data);
			shutdown(session->fd(), SHUT_WR);
			readAll(session->fd());
		}
		ensure_equals(readAll("tmp.output"), "hello world!\n");
	}
	
	// It raises an exception if getStartupCommand() is empty.

	/******* User switching tests *******/

	// If 'user' is set
		// and 'user' is 'root'
			TEST_METHOD(20) {
				// It changes the user to the value of 'defaultUser'.
				SETUP_USER_SWITCHING_TEST();
				options.user = "root";
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["default_user"]);
			}

			TEST_METHOD(21) {
				// If 'group' is given, it changes group to the given group name.
				SETUP_USER_SWITCHING_TEST();
				options.user = "root";
				options.group = testConfig["normal_group_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"]);
			}

			TEST_METHOD(22) {
				// If 'group' is set to the root group, it changes group to defaultGroup.
				SETUP_USER_SWITCHING_TEST();
				string rootGroup = groupNameForGid(0);
				options.user = "root";
				options.group = rootGroup.c_str();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["default_group"]);
			}

			// and 'group' is set to '!STARTUP_FILE!'"
				TEST_METHOD(23) {
					// It changes the group to the startup file's group.
					SETUP_USER_SWITCHING_TEST();
					string rootGroup = groupNameForGid(0);
					options.user = "root";
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"]);
				}

				TEST_METHOD(24) {
					// If the startup file is a symlink, then it uses the symlink's group, not the target's group
					SETUP_USER_SWITCHING_TEST();
					string rootGroup = groupNameForGid(0);
					options.user = "root";
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_2"].asString()));
					chown("tmp.wsgi/passenger_wsgi.py.real",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_2"]);
				}

			TEST_METHOD(25) {
				// If 'group' is not given, it changes the group to defaultUser's primary group.
				SETUP_USER_SWITCHING_TEST();
				options.user = "root";
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid),
					primaryGroupFor(testConfig["default_user"].asString()));
			}

		// and 'user' is not 'root'
			TEST_METHOD(29) {
				// It changes the user to the given username.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["normal_user_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["normal_user_1"]);
			}

			TEST_METHOD(30) {
				// If 'group' is given, it changes group to the given group name.
				// It changes the user to the given username.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["normal_user_1"].asCString();
				options.group = testConfig["normal_group_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
			}

			TEST_METHOD(31) {
				// If 'group' is set to the root group, it changes group to defaultGroup.
				// It changes the user to the given username.
				SETUP_USER_SWITCHING_TEST();
				string rootGroup = groupNameForGid(0);
				options.user = testConfig["normal_user_1"].asCString();
				options.group = rootGroup.c_str();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["default_group"].asString());
			}

			// and 'group' is set to '!STARTUP_FILE!'
				TEST_METHOD(32) {
					// It changes the group to the startup file's group.
					SETUP_USER_SWITCHING_TEST();
					options.user = testConfig["normal_user_1"].asCString();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid),
						testConfig["normal_group_1"].asString());
				}

				TEST_METHOD(33) {
					// If the startup file is a symlink, then it uses the
					// symlink's group, not the target's group.
					SETUP_USER_SWITCHING_TEST();
					options.user = testConfig["normal_user_1"].asCString();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_2"].asString()));
					chown("tmp.wsgi/passenger_wsgi.py.real",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid),
						testConfig["normal_group_2"].asString());
				}

			TEST_METHOD(34) {
				// If 'group' is not given, it changes the group to the user's primary group.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["normal_user_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid),
					primaryGroupFor(testConfig["normal_user_1"].asString()));
			}

		// and the given username does not exist
			TEST_METHOD(38) {
				// It changes the user to the value of defaultUser.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["nonexistant_user"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["default_user"].asString());
			}

			TEST_METHOD(39) {
				// If 'group' is given, it changes group to the given group name.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["nonexistant_user"].asCString();
				options.group = testConfig["normal_group_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
			}

			TEST_METHOD(40) {
				// If 'group' is set to the root group, it changes group to defaultGroup.
				SETUP_USER_SWITCHING_TEST();
				string rootGroup = groupNameForGid(0);
				options.user = testConfig["nonexistant_user"].asCString();
				options.group = rootGroup;
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["default_group"].asString());
			}

			// and 'group' is set to '!STARTUP_FILE!'
				TEST_METHOD(41) {
					// It changes the group to the startup file's group.
					SETUP_USER_SWITCHING_TEST();
					options.user = testConfig["nonexistant_user"].asCString();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
				}

				TEST_METHOD(42) {
					// If the startup file is a symlink, then it uses the
					// symlink's group, not the target's group.
					SETUP_USER_SWITCHING_TEST();
					options.user = testConfig["nonexistant_user"].asCString();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_2"].asString()));
					chown("tmp.wsgi/passenger_wsgi.py.real",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_2"].asString());
				}

			TEST_METHOD(43) {
				// If 'group' is not given, it changes the group to defaultUser's primary group.
				SETUP_USER_SWITCHING_TEST();
				options.user = testConfig["nonexistant_user"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid),
					primaryGroupFor(testConfig["default_user"].asString()));
			}

	// If 'user' is not set
		// and the startup file's owner exists
			TEST_METHOD(47) {
				// It changes the user to the owner of the startup file.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					uidFor(testConfig["normal_user_1"].asString()),
					(gid_t) -1);
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["normal_user_1"].asString());
			}

			TEST_METHOD(48) {
				// If the startup file is a symlink, then it uses the symlink's owner, not the target's owner.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					uidFor(testConfig["normal_user_2"].asString()),
					(gid_t) -1);
				chown("tmp.wsgi/passenger_wsgi.py.real",
					uidFor(testConfig["normal_user_1"].asString()),
					(gid_t) -1);
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["normal_user_2"].asString());
			}

			TEST_METHOD(49) {
				// If 'group' is given, it changes group to the given group name.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					uidFor(testConfig["normal_user_1"].asString()),
					(gid_t) -1);
				options.group = testConfig["normal_group_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
			}

			TEST_METHOD(50) {
				// If 'group' is set to the root group, it changes group to defaultGroup.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					uidFor(testConfig["normal_user_1"].asString()),
					(gid_t) -1);
				string rootGroup = groupNameForGid(0);
				options.group = rootGroup;
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["default_group"].asString());
			}

			// and 'group' is set to '!STARTUP_FILE!'
				TEST_METHOD(51) {
					// It changes the group to the startup file's group.
					SETUP_USER_SWITCHING_TEST();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
				}

				TEST_METHOD(52) {
					// If the startup file is a symlink, then it uses the symlink's
					// group, not the target's group.
					SETUP_USER_SWITCHING_TEST();
					options.group = "!STARTUP_FILE!";
					lchown("tmp.wsgi/passenger_wsgi.py",
						(uid_t) -1,
						gidFor(testConfig["normal_group_2"].asString()));
					chown("tmp.wsgi/passenger_wsgi.py.real",
						(uid_t) -1,
						gidFor(testConfig["normal_group_1"].asString()));
					RUN_USER_SWITCHING_TEST();
					ensure_equals(groupNameForGid(gid), testConfig["normal_group_2"].asString());
				}

			TEST_METHOD(53) {
				// If 'group' is not given, it changes the group to the startup file's owner's primary group.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					uidFor(testConfig["normal_user_1"].asString()),
					(gid_t) -1);
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid),
					primaryGroupFor(testConfig["normal_user_1"].asString()));
			}

		// and the startup file's owner doesn't exist
			TEST_METHOD(57) {
				// It changes the user to the value of defaultUser.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					(uid_t) testConfig["nonexistant_uid"].asInt64(),
					(gid_t) -1);
				RUN_USER_SWITCHING_TEST();
				ensure_equals(userNameForUid(uid), testConfig["default_user"].asString());
			}

			TEST_METHOD(58) {
				// If 'group' is given, it changes group to the given group name.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					(uid_t) testConfig["nonexistant_uid"].asInt64(),
					(gid_t) -1);
				options.group = testConfig["normal_group_1"].asCString();
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
			}

			TEST_METHOD(59) {
				// If 'group' is set to the root group, it changes group to defaultGroup.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					(uid_t) testConfig["nonexistant_uid"].asInt64(),
					(gid_t) -1);
				string rootGroup = groupNameForGid(0);
				options.group = rootGroup;
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), testConfig["default_group"].asString());
			}

			// and 'group' is set to '!STARTUP_FILE!'
				// and the startup file's group doesn't exist
					TEST_METHOD(60) {
						// It changes the group to the value given by defaultGroup.
						SETUP_USER_SWITCHING_TEST();
						lchown("tmp.wsgi/passenger_wsgi.py",
							(uid_t) testConfig["nonexistant_uid"].asInt64(),
							(gid_t) testConfig["nonexistant_gid"].asInt64());
						options.group = "!STARTUP_FILE!";
						RUN_USER_SWITCHING_TEST();
						ensure_equals(groupNameForGid(gid), testConfig["default_group"].asString());
					}

				// and the startup file's group exists
					TEST_METHOD(61) {
						// It changes the group to the startup file's group.
						SETUP_USER_SWITCHING_TEST();
						lchown("tmp.wsgi/passenger_wsgi.py",
							(uid_t) testConfig["nonexistant_uid"].asInt64(),
							gidFor(testConfig["normal_group_1"].asString()));
						options.group = "!STARTUP_FILE!";
						RUN_USER_SWITCHING_TEST();
						ensure_equals(groupNameForGid(gid), testConfig["normal_group_1"].asString());
					}

					TEST_METHOD(62) {
						// If the startup file is a symlink, then it uses the symlink's group, not the target's group.
						SETUP_USER_SWITCHING_TEST();
						lchown("tmp.wsgi/passenger_wsgi.py",
							(uid_t) testConfig["nonexistant_uid"].asInt64(),
							gidFor(testConfig["normal_group_2"].asString()));
						chown("tmp.wsgi/passenger_wsgi.py.real",
							(uid_t) -1,
							gidFor(testConfig["normal_group_1"].asString()));
						options.group = "!STARTUP_FILE!";
						RUN_USER_SWITCHING_TEST();
						ensure_equals(groupNameForGid(gid), testConfig["normal_group_2"].asString());
					}

			TEST_METHOD(63) {
				// If 'group' is not given, it changes the group to defaultUser's primary group.
				SETUP_USER_SWITCHING_TEST();
				lchown("tmp.wsgi/passenger_wsgi.py",
					(uid_t) testConfig["nonexistant_uid"].asInt64(),
					(gid_t) -1);
				RUN_USER_SWITCHING_TEST();
				ensure_equals(groupNameForGid(gid), primaryGroupFor(testConfig["default_user"].asString()));
			}

	TEST_METHOD(67) {
		// It raises an error if it tries to lower to 'defaultUser',
		// but that user doesn't exist.
		SETUP_USER_SWITCHING_TEST();
		options.user = "root";
		options.defaultUser = testConfig["nonexistant_user"].asCString();
		try {
			RUN_USER_SWITCHING_TEST();
			fail();
		} catch (const RuntimeException &e) {
			ensure(containsSubstring(e.what(), "Cannot determine a user to lower privilege to"));
		}
	}

	TEST_METHOD(68) {
		// It raises an error if it tries to lower to 'default_group',
		// but that group doesn't exist.
		SETUP_USER_SWITCHING_TEST();
		options.user = testConfig["normal_user_1"].asCString();
		options.group = groupNameForGid(0);
		options.defaultGroup = testConfig["nonexistant_group"].asCString();
		try {
			RUN_USER_SWITCHING_TEST();
			fail();
		} catch (const RuntimeException &e) {
			ensure(containsSubstring(e.what(), "Cannot determine a group to lower privilege to"));
		}
	}

	TEST_METHOD(69) {
		// Changes supplementary groups to the owner's default supplementary groups.
		SETUP_USER_SWITCHING_TEST();
		options.user = testConfig["normal_user_1"].asCString();
		RUN_USER_SWITCHING_TEST();
		runShellCommand(("groups " + testConfig["normal_user_1"].asString() + " > /tmp/info2.txt").c_str());
		string defaultGroups = strip(readAll("/tmp/info2.txt"));
		// default_groups.gsub!(/.*: */, '')
		ensure_equals(groups, defaultGroups);
	}
