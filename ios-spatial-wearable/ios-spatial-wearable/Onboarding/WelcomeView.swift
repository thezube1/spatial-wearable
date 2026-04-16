import SwiftUI

struct WelcomeView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(AuthViewModel.self) private var auth

    @State private var name = ""
    @State private var email = ""
    @State private var username = ""
    @State private var password = ""
    @State private var isLoginMode = false

    /// Figma `113:29`: centered, 24pt line height, max width 178; sign-up copy matches artboard.
    @ViewBuilder
    private var welcomeTitleView: some View {
        if isLoginMode {
            Text("Welcome back")
                .font(OnboardingStyle.font(24, weight: .bold))
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 178)
                .lineSpacing(0)
                .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)
        } else {
            Text("Welcome to Tether")
                .font(OnboardingStyle.font(24, weight: .bold))
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 240)
                .lineSpacing(0)
                .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)
        }
    }

    var body: some View {
        ZStack {
            WelcomeHeroBackground()

            GeometryReader { geo in
                let topInset = geo.safeAreaInsets.top
                let bottomInset = geo.safeAreaInsets.bottom
                /// Logo top at y=132 in Figma (full frame).
                let logoTopPadding = max(0, 132 - topInset)
                /// Space below button in Figma: 852 − (653 + 40) = 159.
                let bottomPadding = max(24, 159 - bottomInset)

                ScrollView(showsIndicators: false) {
                    VStack(spacing: 0) {
                        Color.clear.frame(height: logoTopPadding)

                        ZStack {
                            Circle()
                                .fill(Color.white.opacity(0.78))
                                .frame(width: 107, height: 107)
                            Image("WelcomeLogo")
                                .renderingMode(.original)
                                .resizable()
                                .interpolation(.high)
                                .scaledToFit()
                                .frame(width: 62, height: 62)
                        }
                        /// 262 − (132 + 107) = 23
                        .padding(.bottom, 23)

                        welcomeTitleView
                            /// 321 − (262 + 24) = 35
                            .padding(.bottom, 35)

                        VStack(spacing: 30) {
                            if !isLoginMode {
                                TextField("", text: $name, prompt: Text("Name").foregroundColor(.black.opacity(0.55)))
                                    .textContentType(.name)
                                    .welcomeSignUpField()
                            }
                            TextField("", text: $email, prompt: Text("Email").foregroundColor(.black.opacity(0.55)))
                                .textContentType(.emailAddress)
                                .keyboardType(.emailAddress)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .welcomeSignUpField()
                            if !isLoginMode {
                                TextField("", text: $username, prompt: Text("Username").foregroundColor(.black.opacity(0.55)))
                                    .textInputAutocapitalization(.never)
                                    .autocorrectionDisabled()
                                    .welcomeSignUpField()
                            }
                            SecureField("", text: $password, prompt: Text("Password").foregroundColor(.black.opacity(0.55)))
                                .welcomeSignUpField()
                        }

                        if let err = auth.errorMessage {
                            Text(err)
                                .font(OnboardingStyle.font(12))
                                .foregroundStyle(.red.opacity(0.95))
                                .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                                .padding(.top, 16)
                        }

                        /// 653 − (321 + 250) = 82
                        Color.clear
                            .frame(height: auth.errorMessage == nil ? 82 : 56)

                        Button {
                            Task { await submit() }
                        } label: {
                            if auth.isLoading {
                                ProgressView().tint(.white)
                            } else {
                                Text("Get Started")
                            }
                        }
                        .buttonStyle(WelcomePrimaryButtonStyle())
                        .disabled(auth.isLoading)

                        Button {
                            isLoginMode.toggle()
                        } label: {
                            Text(isLoginMode
                                 ? "New here? Create an account"
                                 : "Already have an account? Log in")
                                .font(OnboardingStyle.font(13))
                                .foregroundStyle(.white.opacity(0.92))
                                .underline()
                        }
                        .padding(.top, 20)

                        Color.clear.frame(height: bottomPadding)
                    }
                    .frame(maxWidth: .infinity)
                    .frame(minHeight: geo.size.height)
                    .padding(.horizontal, 28)
                }
            }

            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()
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
