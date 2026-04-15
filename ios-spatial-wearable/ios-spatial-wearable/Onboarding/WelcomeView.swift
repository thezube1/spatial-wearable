import SwiftUI

struct WelcomeView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(AuthViewModel.self) private var auth

    @State private var name = ""
    @State private var email = ""
    @State private var username = ""
    @State private var password = ""
    @State private var isLoginMode = false

    var body: some View {
        ZStack {
            OnboardingBackground()

            ScrollView {
                VStack(spacing: 24) {
                    Spacer(minLength: 40)

                    // Logo placeholder — stands in for the radial design.
                    ZStack {
                        Circle().stroke(Color.white.opacity(0.4), lineWidth: 1).frame(width: 140, height: 140)
                        Circle().stroke(Color.white.opacity(0.25), lineWidth: 1).frame(width: 100, height: 100)
                        Image(systemName: "circle.hexagongrid.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.white)
                    }
                    .padding(.bottom, 8)

                    VStack(spacing: 6) {
                        Text(isLoginMode ? "Welcome back." : "Let's get you set up.")
                            .font(OnboardingStyle.font(24, weight: .bold))
                            .foregroundStyle(.white)
                        Text(isLoginMode ? "Sign in to continue." : "Create your account to begin.")
                            .font(OnboardingStyle.font(14))
                            .foregroundStyle(.white.opacity(0.7))
                    }

                    VStack(spacing: 12) {
                        if !isLoginMode {
                            TextField("", text: $name, prompt: Text("Name").foregroundColor(.white.opacity(0.6)))
                                .textContentType(.name)
                                .onboardingField()
                        }
                        TextField("", text: $email, prompt: Text("Email").foregroundColor(.white.opacity(0.6)))
                            .textContentType(.emailAddress)
                            .keyboardType(.emailAddress)
                            .autocapitalization(.none)
                            .onboardingField()
                        if !isLoginMode {
                            TextField("", text: $username, prompt: Text("Username").foregroundColor(.white.opacity(0.6)))
                                .autocapitalization(.none)
                                .onboardingField()
                        }
                        SecureField("", text: $password, prompt: Text("Password").foregroundColor(.white.opacity(0.6)))
                            .onboardingField()
                    }

                    if let err = auth.errorMessage {
                        Text(err)
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.red.opacity(0.9))
                            .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                    }

                    Button {
                        Task { await submit() }
                    } label: {
                        if auth.isLoading {
                            ProgressView().tint(.white)
                        } else {
                            Text("Get Started")
                        }
                    }
                    .buttonStyle(PrimaryBlueButtonStyle())
                    .disabled(auth.isLoading)

                    Button {
                        isLoginMode.toggle()
                    } label: {
                        Text(isLoginMode
                             ? "New here? Create an account"
                             : "Already have an account? Log in")
                            .font(OnboardingStyle.font(13))
                            .foregroundStyle(.white.opacity(0.85))
                            .underline()
                    }

                    Spacer(minLength: 40)
                }
                .padding(.vertical, 24)
            }
        }
    }

    private func submit() async {
        if isLoginMode {
            await auth.signIn(email: email, password: password)
        } else {
            await auth.signUp(email: email, password: password,
                              username: username, displayName: name)
        }
        if auth.currentSession != nil {
            coordinator.step = .linkWristband
        }
    }
}

#Preview {
    WelcomeView()
        .environment(OnboardingCoordinator())
        .environment(AuthViewModel())
}
